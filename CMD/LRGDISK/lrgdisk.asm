;=====================================================================
; LRGDISK.ASM - LRGDISK.386, 32-bit disk access for Windows 3.1 and
; Windows for Workgroups 3.11 on IDE disks past 504MB, by LBA, with
; bus-master DMA, on both IDE cables
;
;   [386Enh]
;   device=lrgdisk.386          in place of device=*wdctrl
;   32BitDiskAccess=on          as for WDCTRL; off keeps LRGDISK out
;   LRGDiskDMA=off              optional: PIO only, no bus-master DMA
;   LRGDiskSerial=on            optional: the two cables one at a time
;                               (done on a CMD640 without asking)
;   LRGDiskBlockMode=off        optional: PIO a sector a command block,
;                               no READ or WRITE MULTIPLE
;   LRGDiskPoll=off             optional: an interrupt for every block
;                               of a PIO transfer
;   LRGDisk32BitIO=off          optional: the data port 16 bits at a
;                               time
;   LRGDiskReport=on            optional: what LRGDISK found, written
;                               to LRGDISK.TXT in the boot drive's
;                               root every time Windows starts
;
; device=*int13 and device=*blockdev stay where they are: LRGDISK is a
; disk driver for BlockDev, as WDCTRL is, and INT13.386 is what sends
; a DOS box's INT 13h to it.
;
; WHY WDCTRL STOPS AT 504MB.  WDCTRL drives the controller with the
; geometry in the BIOS's fixed disk table (INT 41h, INT 46h), which is
; the geometry INT 13h presents - and over 1024 cylinders x 16 heads
; x 63 sectors that is no longer the drive's.  The BIOS makes up a
; geometry with more heads and fewer cylinders for INT 13h's ten-bit
; cylinder number, and turns every address back into the drive's own
; - an LBA, or the CHS of the drive's real 16 heads - before it
; touches a port.  WDCTRL's start-up sees the difference (its check
; reads the controller's registers after a BIOS read, and a head
; number of 40 does not fit in four bits) and refuses the drive.
;
; LRGDISK keeps the BIOS's geometry for everything Windows sees -
; INT13.386 turns a DOS box's CHS into a sector number with it, and
; answers AH=08h with it - and talks to the drive by LBA: 28 bits of
; it, and 48 for a transfer that reaches past the first 128GB of a
; drive that has LBA48 (READ/WRITE SECTORS EXT), to 2TB - a sector
; number in DOS and in a partition table is 32 bits.  LBA wherever
; it reaches the same sectors the BIOS does: when the BIOS uses it, and
; when the BIOS's CHS is the drive's own geometry.  CHS, the BIOS's,
; only for a drive without LBA.  None of it is guessed.  The real-mode
; start-up below reads four sectors through the BIOS, looks at what the
; BIOS left in the controller's registers each time, works out from
; them how the BIOS addresses the drive, reads the same sectors itself
; and compares - and by LBA, the drive's last sector too, through the
; BIOS's extensions if it has them.  Anything it cannot account for
; leaves the drive to the BIOS.
;
; TWO CABLES.  The BIOS's disks 80h to 83h, wherever each is: either
; unit of the first cable (1F0h, IRQ 14) or of the second (170h,
; IRQ 15).  Which cable and unit a BIOS disk is comes from where the
; BIOS's own read of its first sector went.  Each cable has a state of
; its own - the command on it, the DOS programs' view of it - so that
; one cable waits for nothing on the other.  BlockDev owns IRQ 14 and
; calls LRGDISK for it; IRQ 15 LRGDISK takes from VPICD itself.
;
; INT 13h.  INT13.386 sends a DOS box's INT 13h for a drive with a
; 32-bit driver to BlockDev - for disks 80h and 81h, and from a table
; that stops at AH=1Ah, so under WDCTRL AH=42h fails - and RMDOS
; reaches everything past cylinder 1023 with AH=42h: a FAT32 volume
; past 8GB.  LRGDISK hooks INT13.386's translation (ld_xlate) and
; answers AH=41h-48h for its drives itself, through BlockDev; for a
; drive numbered 82h or 83h, which INT13.386 never looks at, it
; answers the whole of INT 13h.
;
; BUS-MASTER DMA.  A PCI IDE controller has an engine for each cable
; (BAR4: eight ports a cable) that moves the data itself from a table
; of physical addresses: one interrupt a command instead of one a
; sector, and the processor free meanwhile.  LRGDISK uses it for a
; drive that has a DMA mode SELECTED - Ultra DMA, or multiword - by
; whoever set the drive and the controller's timing up before Windows
; started: the BIOS, or PM-DOS's kernel (IDEDMA=).  It sets no mode of
; its own: the drive's end and the controller's have to agree, and the
; controller's is a different register in every chipset.  So the speed
; is whatever was in force at the DOS prompt: with the BIOS's "Ultra
; DMA mode 2" or the kernel's IDEDMA=ON, that.  A transfer whose
; buffer the engine cannot be pointed at (an odd address, a page not
; present, more pieces than the table holds) goes by PIO instead, and
; a drive whose engine gets a transfer wrong once is PIO from then on.
; LRGDiskDMA=off keeps the engine out of it.
;
; PIO, WITH FEWER INTERRUPTS.  Without the engine WDCTRL's way is one
; sector an interrupt, a word at a time, and under Windows the
; interrupt costs more than the sector does: VPICD, BlockDev and back
; for every 512 bytes.  Three things, each with a switch.
;
; Block mode (LRGDiskBlockMode).  READ and WRITE MULTIPLE move a block
; of sectors for one interrupt.  The block is the size the drive is
; set to (IDENTIFY word 59): the BIOS set it and reads with it, so it
; is never changed.  A drive set to none is set to the most it takes
; (word 47), LD_BLOCKMAX at most - which a BIOS that reads a sector at
; a time never notices, READ SECTORS being what it always was.  A DOS
; program's reset of the cable loses the size, and ld_reinit gives it
; back.  A block-mode command that ends in an error is tried again a
; sector at a time, and that drive stays so.  And the size is not
; taken on trust: whichever it is, the drive is given it as a command
; before any block is asked for (ld_setmult) - the drive's taking the
; command is what counts, not what IDENTIFY says afterwards - and
; inside a block each sector after the first is moved only while the
; drive holds DRQ up for it (ld_xfer): a block shorter than the size
; says ends where the drive ends it, not in data that was never there.
;
; Staying in the handler (LRGDiskPoll).  A drive with the next block
; in its buffer has it ready microseconds after the last was taken,
; and so has one just given a block to write.  After a block the
; handler looks at the alternate status LD_POLLN times - about what
; one interrupt costs - and if the drive is ready, moves the next
; block there and then: LD_BURST sectors at most for one interrupt.
; The request such a block raises is dropped by the read of the
; status, made with interrupts off, and never reaches the processor.
;
; 32-bit data (LRGDisk32BitIO).  REP INSD and REP OUTSD in place of
; REP INSW and REP OUTSW: half the bus cycles.  The data register is
; 16 bits wide at the drive; it is the host adapter that latches two
; words, a VLB and PCI thing - on an ISA card a doubleword read of
; 1F0h is a word from 1F0h and a word from 1F2h, and no error says so.
; So it is tried only where the drive says it can (IDENTIFY word 48)
; or the controller is a PCI one, and then PROVED: the IDENTIFY block,
; read a word at a time and a doubleword at a time, must be the same
; 512 bytes with nothing left in the drive (ld_prove32, which is the
; kernel's ata_prove32).  A write cannot be proved without writing
; one: it goes on the read's word, as it does in the kernel.  Never on
; a CMD640, whose 32-bit path is the read-ahead that is switched off.
;
; THE HANDLER GOES BY THE DRIVE'S STATUS, not by the fact of an
; interrupt.  A block taken by looking leaves its request behind in an
; interrupt controller that latches the edge (QEMU's does; an 8259A
; forgets a request withdrawn before it is acknowledged), and that
; interrupt comes later with nothing to do.  So: busy, or no data
; wanted and none owed, is not this interrupt's, and whatever the
; drive does want is done whichever interrupt it was.  CBL_GO says a
; controller command is out - and, for a write, its first block sent
; - and nothing is touched without it.  Between the sectors of a
; block the handler lets other interrupts in (CBL_INXFER keeps its own
; out), so nothing waits longer for the disk than it did at one sector
; an interrupt.  And a command the controller has had for LD_LOSTMS,
; with the drive no longer busy, is taken up from the time-out as if
; its interrupt had come.
;
; THE CABLE, SHARED.  LRGDISK takes a cable's ports from the VMs as
; WDCTRL does.  WDCTRL keeps them: a CD-ROM on the same cable is out of
; its DOS driver's reach.  LRGDISK lends them (ld_vmio): with something
; it does not drive on the cable - a CD-ROM drive, or a disk the checks
; left to the BIOS - the VMs' port I/O reaches the controller whenever
; LRGDISK has no command on it, and one command at a time either way:
; a command of theirs holds LRGDISK's next until it is over, and one
; written while LRGDISK's is on the cable waits for it.  Their
; device's interrupt goes to the VM whose command is on the cable - to
; the BIOS's own handler in it, or to a driver's that hooked IRQ 14 or
; 15 - which is what the VM would get with no 32-bit disk access at
; all; VirtualHDIrq need not be set.  A reset a DOS program writes to
; the cable (SRST) resets LRGDISK's drives too: before its next command
; on that cable LRGDISK gives each of them its parameters again - the
; BIOS's geometry to a drive used by CHS, the transfer mode to one
; used by DMA, the block's size to one used in blocks.  LRGDISK's own
; drives stay out of the VMs' reach, as before.
;
; TWO CHIPS WITH BUGS.  The PC-Tech RZ1000 corrupts a PIO read when its
; read-ahead is on and the processor is interrupted between sectors -
; which under Windows it always is - and the CMD640 corrupts data when
; its two cables transfer at once, and with read-ahead on.  Neither has
; a bus-master engine.  Both are found through the PCI BIOS at start-up
; and their read-ahead switched off, as Linux does, and on a CMD640 the
; two cables are used one at a time (ld_serial: a command, LRGDISK's or
; a DOS program's, waits while the other cable has one).  LRGDiskSerial
; =on asks for that on any controller.  The VLB CMD640 is not looked
; for.
;
; WHAT IS NOT HANDLED.  A BIOS that translates with CHS on a drive with
; no LBA is refused (nothing made since 1994 is one).  Past 2TB, the
; first 2TB.  A cable with none of LRGDISK's drives on it is left
; alone entirely.  A third or fourth IDE channel (1E8h, 168h) is the
; BIOS's.
;
; HOW IT IS BUILT.  An LE VxD as the Windows 3.1 DDK's LINK386 makes
; one: locked code and data, code discarded after start-up, a 16-bit
; real-mode start-up object as the module's entry, and the descriptor
; block exported as ordinal 1.  wlink's "format windows vxd" lays it
; out the same way.  One difference: LINK386 resolves the 16-bit
; object's own offsets and leaves it with no fixups, and wlink would
; leave them for the loader - which may or may not apply them to code
; it runs in real mode.  So the real-mode code never names an address
; directly: every one is written "label-rbase", a difference two
; labels in one segment make, which the assembler resolves itself.
;
; Linked with:  wlink format windows vxd, see BUILD.CMD
;=====================================================================
        .386p
        .model  flat

_LTEXT  SEGMENT DWORD USE32 PUBLIC 'CODE'
_LTEXT  ENDS
_LDATA  SEGMENT DWORD USE32 PUBLIC 'CODE'
_LDATA  ENDS
_ITEXT  SEGMENT DWORD USE32 PUBLIC 'ICODE'
_ITEXT  ENDS
_IDATA  SEGMENT DWORD USE32 PUBLIC 'ICODE'
_IDATA  ENDS
_RCODE  SEGMENT WORD USE16 PUBLIC 'RCODE'
_RCODE  ENDS

_LGROUP GROUP   _LTEXT, _LDATA
_IGROUP GROUP   _ITEXT, _IDATA

;---------------------------------------------------------------------
; Services, as INT 20h and the device and service numbers after it.
; The numbers are the Windows 3.1 DDK's (VMM.INC, BLOCKDEV.INC,
; VPICD.INC, SHELL.INC); 3.11 only adds to the ends of the tables.
;---------------------------------------------------------------------
VXDCALL MACRO   svc
        int     20h
        dd      svc
        ENDM

S_ENABLE_VM_INTS        EQU     0001001Ah
S_CREATE_SEM            EQU     00010025h
S_DESTROY_SEM           EQU     00010026h
S_WAIT_SEM              EQU     00010027h
S_SIGNAL_SEM            EQU     00010028h
S_SET_GLOBAL_TIME_OUT   EQU     0001003Ch
S_SIMULATE_IRET         EQU     00010047h
S_BUILD_INT_STACK_FRAME EQU     0001004Ch
S_PAGEALLOCATE          EQU     00010053h       ; C convention
S_COPYPAGETABLE         EQU     00010061h       ; C convention
S_LINPAGELOCK           EQU     00010063h       ; C convention
S_LINPAGEUNLOCK         EQU     00010064h       ; C convention
S_BEGIN_NEST_V86_EXEC   EQU     00010082h
S_EXEC_INT              EQU     00010084h
S_RESUME_EXEC           EQU     00010085h
S_END_NEST_EXEC         EQU     00010086h
S_HOOK_DEVICE_SERVICE   EQU     00010090h
S_SIMULATE_IO           EQU     00010094h
S_INSTALL_IO_HANDLER    EQU     00010096h
S_GET_PROFILE_BOOLEAN   EQU     000100B1h       ; start-up only
S_FATAL_ERROR_HANDLER   EQU     000100BEh
S_GET_TIME              EQU     000100CFh       ; Get_Last_Updated_System_Time
S_VPICD_VIRTUALIZE_IRQ  EQU     00030001h
S_VPICD_SET_INT_REQ     EQU     00030002h
S_VPICD_CLEAR_INT_REQ   EQU     00030003h
S_VPICD_PHYS_EOI        EQU     00030004h
S_VPICD_PHYS_UNMASK     EQU     00030009h
S_BD_REGISTER_DEVICE    EQU     00100001h
S_BD_SEND_COMMAND       EQU     00100004h
S_BD_COMMAND_COMPLETE   EQU     00100005h
S_SHELL_SYSMODAL_MSG    EQU     00170003h
S_INT13_TRANSLATE       EQU     00200002h       ; Int13_Translate_VM_Int

BLOCK_SVC_INTS          EQU     1               ; Wait_Semaphore: other
BLOCK_ENABLE_INTS       EQU     4               ; interrupts go on

PG_SYS                  EQU     1               ; _PageAllocate: everyone's
PAGEZEROINIT            EQU     01h
PAGEFIXED               EQU     08h
P_PRES                  EQU     01h             ; a page table entry's

; A VM's registers (Client_Reg_Struc) and its control block
CL_ESI          EQU     04h
CL_SI           EQU     04h
CL_BX           EQU     10h
CL_DX           EQU     14h
CL_DL           EQU     14h
CL_DH           EQU     15h
CL_CX           EQU     18h
CL_CL           EQU     18h
CL_CH           EQU     19h
CL_AX           EQU     1Ch
CL_AL           EQU     1Ch
CL_AH           EQU     1Dh
CL_EFLAGS       EQU     2Ch
CL_ES           EQU     38h
CL_DS           EQU     3Ch
CB_HIGHLIN      EQU     04h             ; CB_High_Linear
CB_CLIENTPTR    EQU     08h             ; CB_Client_Pointer

SYS_CRITICAL_INIT       EQU     0               ; the control calls we answer
DEVICE_INIT             EQU     1

BYTE_OUTPUT             EQU     4               ; I/O trap types: 0, 4 bytes,
WORD_INPUT              EQU     8               ; 8, 0Ch words, 10h, 14h dwords;
DWORD_INPUT             EQU     10h             ; 4 set for output
IO_OUTPUT               EQU     4
STRING_IO               EQU     20h
REP_IO                  EQU     40h

MB_ICONEXCLAMATION      EQU     00000030h
MB_SYSTEMMODAL          EQU     00001000h
MB_NOWINDOW             EQU     40000000h
MB_ASAP                 EQU     80000000h
LD_MBFLAGS              EQU     MB_SYSTEMMODAL OR MB_ASAP OR MB_ICONEXCLAMATION OR MB_NOWINDOW

;---------------------------------------------------------------------
; BlockDev's device descriptor (BlockDev_Device_Descriptor, 64h
; bytes), and LRGDISK's own bytes after it
;---------------------------------------------------------------------
BD_MAJOR        EQU     04h             ; db 3
BD_MINOR        EQU     05h             ; db 0Ah
BD_TYPE         EQU     06h             ; db 5, a fixed disk
BD_INT13        EQU     07h             ; db 80h to 83h
BD_FLAGS        EQU     08h             ; dd
BD_NAMEPTR      EQU     0Ch             ; dd
BD_MAXSEC       EQU     10h             ; dq the last sector number
BD_SECSIZE      EQU     18h             ; dd 512
BD_HEADS        EQU     1Ch             ; dd
BD_CYLS         EQU     20h             ; dd
BD_SPT          EQU     24h             ; dd
BD_SYNCPROC     EQU     28h             ; dd
BD_CMDPROC      EQU     2Ch             ; dd
BD_HWINTPROC    EQU     30h             ; dd
BD_SIZE         EQU     64h             ; ...and BlockDev's own 30h bytes

BP_CABLE        EQU     64h             ; dd -> its cable's state
BP_UNIT         EQU     68h             ; 0 master, 1 slave
BP_MODE         EQU     69h             ; 0 CHS, the BIOS's; 1 LBA; 2 LBA
                                        ; and LBA48 past 28 bits
BP_XFER         EQU     6Ah             ; the DMA mode in force, as SET
                                        ; FEATURES names it (40h+n Ultra,
                                        ; 20h+n multiword); 0: PIO
BP_IHEADS       EQU     6Bh             ; INITIALIZE DEVICE PARAMETERS:
BP_ISPT         EQU     6Ch             ; the BIOS's heads less 1, and
                                        ; sectors (a drive used by CHS)
BP_CONTROL      EQU     6Dh             ; the device control register's
BP_MULT         EQU     6Eh             ; sectors a block by PIO (READ and
                                        ; WRITE MULTIPLE); 0: one
BP_IO32         EQU     6Fh             ; the data port is read a
                                        ; doubleword at a time
BP_SIZE         EQU     70h

BDF_INT13       EQU     01h             ; BDD_Flags: an INT 13h drive,
BDF_WRITEABLE   EQU     02h             ; writeable, one command at a
BDF_SERIAL      EQU     10h             ; time - WDCTRL's three

;---------------------------------------------------------------------
; BlockDev's command block (BlockDev_Command_Block)
;---------------------------------------------------------------------
CB_NEXT         EQU     00h             ; dd
CB_COMMAND      EQU     04h             ; dw
CB_STATUS       EQU     06h             ; dw
CB_FLAGS        EQU     08h             ; dd
CB_CPLT         EQU     0Ch             ; dd called with ESI -> the block
CB_SECTOR       EQU     10h             ; dq
CB_COUNT        EQU     18h             ; dd sectors
CB_BUFFER       EQU     1Ch             ; dd linear, or the region list
CB_RESCLIENT    EQU     20h             ; dd the sender's own
CB_SIZE         EQU     2Ch

BDC_READ        EQU     0
BDC_WRITE       EQU     1
BDCF_HIGH       EQU     01h             ; high priority
BDCF_SG         EQU     02h             ; scatter/gather: CB_BUFFER is a
                                        ; list of (sectors, linear) pairs
                                        ; that ends with a count of 0
BDS_SUCCESS     EQU     0
BDS_RETRIED     EQU     1
BDS_CORRECTED   EQU     2
BDS_FIRST_ERR   EQU     10h             ; this and above are failures
BDS_BAD_SECTOR  EQU     10h
BDS_MEDIA_ERR   EQU     14h
BDS_DEVICE_ERR  EQU     15h
BDS_BAD_CMD     EQU     16h

;---------------------------------------------------------------------
; A cable's state (ld_cbl0, ld_cbl1): its ports, the command LRGDISK
; has on it, and the DOS programs' view of it.  EBP -> it in every
; routine that works a cable.
;---------------------------------------------------------------------
CBL_DATA        EQU     00h             ; dw 1F0h or 170h: the data port,
                                        ; and +1 features/error, +2 count,
                                        ; +3 sector, +4 and +5 cylinder,
                                        ; +6 drive/head, +7 status/command
CBL_CTRL        EQU     02h             ; dw 3F6h or 376h: read, the same
                                        ; status, leaving the interrupt
                                        ; request; write: nIEN, SRST, HOB
CBL_BM          EQU     04h             ; dw its bus-master engine, 0 none
CBL_FLAGS       EQU     06h             ; db CF_*
CBL_OURS        EQU     07h             ; db bit n: unit n is LRGDISK's
CBL_IRQH        EQU     08h             ; dd VPICD's handle (the second)
; The command being carried out: one at a time for the cable, as
; WDCTRL does it.  CBL_CUR is 0 with nothing in hand and -1 while a
; completion is being called back with another command waiting.
CBL_CUR         EQU     0Ch             ; dd
CBL_CURBDD      EQU     10h             ; dd
CBL_PEND        EQU     14h             ; dd the one waiting, if any
CBL_PENDBDD     EQU     18h             ; dd
CBL_CMDTIME     EQU     1Ch             ; dd when the controller last got
                                        ; a command; 0 when it has none
CBL_PTR         EQU     20h             ; dd where the next sector goes
CBL_LEFT        EQU     24h             ; dd sectors left in the command
CBL_REGLEFT     EQU     28h             ; dd ...in this scatter/gather
                                        ; region
CBL_REGION      EQU     2Ch             ; dd that region's list entry
CBL_NEXT        EQU     30h             ; dd the next controller command's
                                        ; first sector
CBL_VMWAITVM    EQU     34h             ; dd the VM whose command waits
CBL_VMBUSY      EQU     38h             ; dd the VM whose command is on it
CBL_VMWHEN      EQU     3Ch             ; dd since when
CBL_WATCH       EQU     40h             ; dd the time-out that looks at it
CBL_PRD         EQU     44h             ; dd the engine's table, linear
CBL_PRDPHYS     EQU     48h             ; dd ...and physical
CBL_TF          EQU     4Ch             ; db 4: sector, cylinder low and
                                        ; high, drive/head
CBL_BLOCK       EQU     50h             ; db sectors left in this
                                        ; controller command
CBL_ACCUM       EQU     51h             ; db every status seen, ORed
CBL_RETRIES     EQU     52h             ; db
CBL_EXT         EQU     53h             ; db this controller command is
                                        ; 48-bit
CBL_HOB         EQU     54h             ; db ...and its LBA's bits 24-31
CBL_DMA         EQU     55h             ; db ...and goes by the engine
CBL_RESET       EQU     56h             ; db a DOS program reset the cable:
                                        ; the drives are told their
                                        ; parameters again first
CBL_HELD        EQU     57h             ; db LRGDISK's command waits for a
                                        ; DOS program's to be over
CBL_HWVM        EQU     58h             ; db the controller holds what the
                                        ; DOS programs left in it
CBL_VMWAIT      EQU     59h             ; db a DOS program's command waits
CBL_VMCMD       EQU     5Ah             ; db ...its command byte
CBL_VMSRST      EQU     5Bh             ; db ...or it is a reset
CBL_VW          EQU     5Ch             ; db 6 written to +1 to +6
CBL_VWHOB       EQU     62h             ; db 5 the write before, +1 to +5
CBL_VCTL        EQU     67h             ; db written to the control port
CBL_VR          EQU     68h             ; db 6 +1 to +6 as a read would find
CBL_VST         EQU     6Eh             ; db the status, as last seen
CBL_NUM         EQU     6Fh             ; db 0 or 1, for the trace
CBL_BMST        EQU     70h             ; db the engine's status at the
                                        ; interrupt
CBL_XHELD       EQU     71h             ; db LRGDISK's command waits for
                                        ; the other cable to be idle
CBL_GO          EQU     72h             ; db a controller command is out,
                                        ; and a write's first block sent:
                                        ; the drive's status is the
                                        ; command's
CBL_INXFER      EQU     73h             ; db a block is being moved, or
                                        ; interrupts are being let in
                                        ; after one
CBL_MULT        EQU     74h             ; db sectors a block in this
                                        ; controller command; 1 without
                                        ; block mode
CBL_BURST       EQU     75h             ; db sectors moved for this
                                        ; interrupt
CBL_SIZE        EQU     78h

CF_PRESENT      EQU     01h             ; LRGDISK has a drive on it
CF_OTHER        EQU     02h             ; ...and something it does not
                                        ; drive is there too: shared

;---------------------------------------------------------------------
; What the real-mode start-up hands the protected-mode side (the
; reference data, EDX): a byte for each unit, the first cable's master
; in bits 0-7 and its slave in 8-15, then the second cable's.  Nothing
; more can be handed over: by the time a real-mode start-up runs,
; WIN386's loader owns all of DOS memory and all of extended memory,
; and reuses the start-up's own segment for the next device's.  So
; the measurements are made again in protected mode (ld_initbdd).
; The real-mode side keeps a record a unit for its own use (UR_*).
;---------------------------------------------------------------------
UR_FLAGS        EQU     00h             ; db RF_*
UR_DISK         EQU     01h             ; db 80h to 83h
UR_HEADS        EQU     02h             ; dw the BIOS's geometry
UR_SPT          EQU     04h             ; db
UR_XFER         EQU     05h             ; db as BP_XFER
UR_CYLS         EQU     06h             ; dw AH=08h's highest plus 2, as
                                        ; INT13.386 wants it
UR_MAXSEC       EQU     08h             ; dd the last sector LRGDISK reaches
UR_IHEADS       EQU     0Ch             ; db as BP_IHEADS, BP_ISPT
UR_ISPT         EQU     0Dh             ; db
UR_SIZE         EQU     10h

RF_OURS         EQU     01h             ; LRGDISK drives it
RF_LBA          EQU     02h             ; by LBA, else by the BIOS's CHS
RF_LBA48        EQU     04h             ; and by LBA48 past 28 bits
RF_OTHER        EQU     08h             ; not LRGDISK's: something else is
                                        ; there, which DOS programs drive
RF_DMA          EQU     10h             ; by the bus-master engine, if the
                                        ; drive has a mode selected
RF_DISKSHIFT    EQU     5               ; bits 5-6: its BIOS disk less 80h
RF_SERIAL       EQU     80h             ; (unit 0's byte) one cable at a
                                        ; time: a CMD640, or
                                        ; LRGDiskSerial=on
RF_DMAON        EQU     80h             ; (unit 1's byte) LRGDiskDMA is
                                        ; not off
RF_PCIIDE       EQU     80h             ; (unit 2's byte) the controller
                                        ; is a PCI one
RF_NO32         EQU     80h             ; (unit 3's byte) a CMD640: never
                                        ; a doubleword at a time

; What a BIOS read's registers are consistent with (rm_classify)
HOW_LBA         EQU     1               ; an LBA that is the sector number
HOW_BIOS        EQU     2               ; the CHS of the BIOS's geometry
HOW_DRIVE       EQU     4               ; the CHS of the drive's set one
HOW_ALL         EQU     7

;---------------------------------------------------------------------
; The controller
;---------------------------------------------------------------------
IDE0_DATA       EQU     1F0h            ; the first cable
IDE0_CTRL       EQU     3F6h
IDE1_DATA       EQU     170h            ; the second
IDE1_CTRL       EQU     376h

PCI_CFG_ADDR    EQU     0CF8h           ; configuration mechanism 1
PCI_CFG_DATA    EQU     0CFCh
PCIC_IOEN       EQU     0001h           ; the command register: I/O
PCIC_BUSMASTER  EQU     0004h           ; cycles answered, the bus mastered

R_FEAT          EQU     1               ; the registers, from the data port
R_COUNT         EQU     2
R_SECTOR        EQU     3
R_CYLLO         EQU     4
R_CYLHI         EQU     5
R_DRVHD         EQU     6
R_STATUS        EQU     7               ; read: status, and the drive's
R_CMD           EQU     7               ; interrupt request goes away
R_CTRL          EQU     8               ; (ld_vmio's index for the control
                                        ; port)

ST_ERR          EQU     01h
ST_IDX          EQU     02h
ST_CORR         EQU     04h
ST_DRQ          EQU     08h
ST_DRDY         EQU     40h
ST_BSY          EQU     80h

CTL_NIEN        EQU     02h             ; device control: no interrupts,
CTL_SRST        EQU     04h             ; reset both devices, read the
CTL_HOB         EQU     80h             ; registers' high-order bytes

ATA_READ        EQU     20h
ATA_READ_EXT    EQU     24h             ; 48-bit LBA: every register is
ATA_WRITE       EQU     30h             ; written twice, the high-order
ATA_WRITE_EXT   EQU     34h             ; byte first
ATA_READ_DMA    EQU     0C8h
ATA_READ_DMAX   EQU     25h
ATA_WRITE_DMA   EQU     0CAh
ATA_WRITE_DMAX  EQU     35h
ATA_READ_MULT   EQU     0C4h            ; READ and WRITE MULTIPLE: a block
ATA_WRITE_MULT  EQU     0C5h            ; of sectors an interrupt
ATA_READ_MULTX  EQU     29h             ; ...and 48-bit
ATA_WRITE_MULTX EQU     39h
ATA_SETMULT     EQU     0C6h            ; SET MULTIPLE MODE: the block's size
ATA_INITPARM    EQU     91h             ; INITIALIZE DEVICE PARAMETERS
ATA_IDENTIFY    EQU     0ECh
ATA_SETFEAT     EQU     0EFh
SF_XFERMODE     EQU     03h             ; SET FEATURES: the transfer mode

; The bus-master engine: eight ports a cable, the second cable's eight
; past the first's
BM_CMD          EQU     0               ; db: start, direction
BM_STATUS       EQU     2               ; db: active, error, interrupt
BM_PRDT         EQU     4               ; dd: the table's physical address
BM_CM_START     EQU     01h
BM_CM_READ      EQU     08h             ; the engine writes memory: a READ
BM_ST_ACTIVE    EQU     01h
BM_ST_ERR       EQU     02h             ; write 1 to clear
BM_ST_IRQ       EQU     04h             ; write 1 to clear

; Its table: a physical address and a byte count (0: 65536) an entry,
; bit 31 of the second dword ending it; an entry never crosses a 64K
; boundary, nor does the table
PRD_ADDR        EQU     0
PRD_COUNT       EQU     4
PRD_LEN         EQU     8
PRD_EOT         EQU     8000h           ; in the high word of PRD_COUNT
LD_PRDMAX       EQU     120             ; entries a cable: two tables in
                                        ; one page

LBA28_END       EQU     0FFFFFFFh       ; the first sector a 28-bit command
                                        ; cannot name

LD_MAXXFER      EQU     127             ; sectors in one controller command
LD_MAXTRIES     EQU     3               ; retries before a command fails
LD_BUSYMS       EQU     500             ; ms for BSY to go before a command
LD_DRQMS        EQU     500             ; ms for a write's first DRQ
LD_STUCKMS      EQU     20000           ; ms before a command is reported
IFNDEF LD_LOSTMS                        ; (wasm -dLD_LOSTMS=n: a rig that
LD_LOSTMS       EQU     2000            ; loses them on purpose)
ENDIF                                   ; ms before a command's interrupt is
                                        ; taken for lost, and how often the
                                        ; commands are looked at
LD_RESETMS      EQU     10000           ; ms for BSY to go after a reset
LD_WATCHMS      EQU     50              ; how often a lent cable is looked at
LD_LENDMS       EQU     30000           ; ms before it is taken back anyway
IFNDEF LD_POLLN                         ; (wasm -dLD_POLLN=n: a rig whose
LD_POLLN        EQU     128             ; drives answer late)
ENDIF                                   ; looks at the alternate status for
                                        ; the next block: an interrupt's
                                        ; worth of time
LD_BURST        EQU     32              ; sectors for one interrupt, while
                                        ; the drive keeps up
IFNDEF LD_BLOCKMAX                      ; (wasm -dLD_BLOCKMAX=n, with
LD_BLOCKMAX     EQU     16              ; -dLD_TESTSET: a rig)
ENDIF                                   ; the block size LRGDISK sets a
                                        ; drive to, at most

IODELAY MACRO
        jmp     short $+2
        ENDM

;---------------------------------------------------------------------
; LRG_TRACE (wasm -dLRG_TRACE): a character or a line to LPT1 for what
; the protected-mode side does - "r" or "w" a command, and a line for
; anything out of the ordinary.  86Box's text printer keeps it; QEMU's
; -parallel file does.
;---------------------------------------------------------------------
TRC     MACRO   c
IFDEF LRG_TRACE
        push    eax
        mov     al,c
        call    ld_tchar
        pop     eax
ENDIF
        ENDM

TRCHEX  MACRO   val, digits
IFDEF LRG_TRACE
        push    eax
        push    ecx
        mov     eax,val
        mov     cl,digits
        call    ld_thex
        pop     ecx
        pop     eax
ENDIF
        ENDM

; ...and a count of something (ld_st*), which ld_tick writes out as a
; line when any of them has moved
STAT    MACRO   which
IFDEF LRG_TRACE
        inc     dword ptr [ld_st+which*4]
ENDIF
        ENDM

SC_INT          EQU     0               ; interrupts taken for a command
SC_STALE        EQU     1               ; ...with nothing to do
SC_WITHIN       EQU     2               ; ...let in while a block moved
SC_SECTOR       EQU     3               ; sectors moved by PIO
SC_BLOCK        EQU     4               ; blocks they moved in
SC_POLLED       EQU     5               ; blocks found ready by looking
SC_WAITED       EQU     6               ; ...and not: left to an interrupt
SC_IO32         EQU     7               ; sectors moved 32 bits at a time
SC_MULTCMD      EQU     8               ; block-mode controller commands
SC_KICK         EQU     9               ; commands taken up by the time-out
SC_SHORT        EQU     10              ; blocks the drive ended early
SC_COUNT        EQU     11

;=====================================================================
; LOCKED DATA
;=====================================================================
_LDATA  SEGMENT
        PUBLIC  LRGDISK_DDB

; The device descriptor block (VMM.INC's VxD_Desc_Block): no device
; number, so no services and no API - nothing asks for LRGDISK by
; number - and the init order WDCTRL has, after BlockDev and INT13
LRGDISK_DDB     dd      0
                dw      030Ah                   ; the DDK's version
                dw      0                       ; Undefined_Device_ID
                db      1, 3                    ; LRGDISK 1.3
                dw      0
                db      'LRGDISK '
                dd      00D000000h              ; after VPICD (0C000000h);
                                                ; before PageFile (18000000h),
                                                ; whose Device_Init is the
                                                ; first to read the disk, and
                                                ; ld_devinit must have told
                                                ; the drives their geometry
                                                ; by then
                dd      OFFSET FLAT:ld_control
                dd      0, 0, 0, 0
                dd      0
                dd      0, 0

; A descriptor a unit: the first cable's master and slave, then the
; second's.  An unused one has a BIOS disk number of 0.
ld_bdd          db      4*BP_SIZE dup (0)
ld_names        db      'LRGDISK0', 0, 'LRGDISK1', 0
                db      'LRGDISK2', 0, 'LRGDISK3', 0

; The cables
ld_cbl0         db      CBL_SIZE dup (0)
ld_cbl1         db      CBL_SIZE dup (0)

ld_bmbase       dw      0               ; the engine's sixteen ports, for
                                        ; the trap; 0 with no engine
ld_dmaon        db      0               ; the engine may be used
ld_serial       db      0               ; one cable at a time: a command
                                        ; waits while the other cable has
                                        ; one, LRGDISK's or a DOS program's
ld_pollon       db      0               ; after a block, a look for the
                                        ; next before its interrupt
ld_warned       db      0               ; the port message has been shown
ld_tell         db      0               ; ld_vmio refused a command: say so
ld_prev13       dd      0               ; Int13_Translate_VM_Int, hooked
ld_rawbios      db      0               ; ld_xlate: an INT 13h of LRGDISK's
                                        ; own, for the BIOS itself
ld_ptes         dd      20 dup (0)      ; a region's page table entries
ld_bounce       db      512 dup (0)     ; a sector for a buffer that will
                                        ; not lock
IFDEF LRG_TRACE
ld_st           dd      SC_COUNT dup (0) ; the counts (SC_*)
ld_stsaid       dd      0               ; their sum when last written out
ENDIF

; VPICD's descriptor for IRQ 15 (VPICD_IRQ_Descriptor)
ld_vid15        dw      15
                dw      0               ; not shared
                dd      OFFSET FLAT:ld_hwint15
                dd      0               ; no virtual interrupt procedure
                dd      OFFSET FLAT:ld_eoi15
                dd      0               ; mask change
                dd      0               ; IRET
                dd      500             ; ms, VPICD's usual

ld_title        db      'LRGDISK', 0
ld_stuck        db      'A hard disk command has not finished in 20 '
                db      'seconds.  The disk or its controller may have '
                db      'stopped answering.', 0
ld_portmsg      db      'A program tried to use the hard disk '
                db      'controller directly.  Windows is using it, so '
                db      'the program was given nothing and may report a '
                db      'disk error.', 0
_LDATA  ENDS

;=====================================================================
; LOCKED CODE
;=====================================================================
_LTEXT  SEGMENT
        ASSUME  ds:FLAT, es:FLAT, ss:FLAT

;---------------------------------------------------------------------
; ld_control - the device control procedure.  EAX = the call.
;---------------------------------------------------------------------
ld_control:
        cmp     eax,SYS_CRITICAL_INIT
        jne     lc_dev
        jmp     ld_syscrit
lc_dev:
        cmp     eax,DEVICE_INIT
        jne     lc_other
        jmp     ld_devinit
lc_other:
        clc
        ret

;---------------------------------------------------------------------
; ld_sync - BDD_Sync_Cmd_Proc.  AX=0 is "what version", the one call
; there is.
;---------------------------------------------------------------------
ld_sync:
        or      ax,ax
        jnz     lsy_bad
        mov     ax,0100h
        clc
        ret
lsy_bad:
        mov     ax,1                    ; BD_SC_Err_Invalid_Cmd
        stc
        ret

;---------------------------------------------------------------------
; ld_command - BDD_Command_Proc: ESI -> command block, EDI -> the
; drive's descriptor, interrupts off.  BlockDev sends one command a
; drive at a time and has checked the sector numbers.  The other
; drive's command on the same cable while one is in hand waits in
; CBL_PEND; one that arrives while a completion is being called back
; may go ahead of the waiting one if it is high priority and the
; waiting one is not.
;
; Also called by ld_hwint to retry a command from its start, with
; CBL_CUR put back to 0, and by ld_done to start the waiting one.
;
; A DOS program's command waiting for the cable goes out first, and
; while one of theirs is on it this one is held (CBL_HELD), to be
; started by ld_release when theirs is over.
;---------------------------------------------------------------------
ld_command:
        pushad
        mov     ebp,[edi+BP_CABLE]
        cmp     word ptr [esi+CB_COMMAND],BDC_WRITE
        ja      ldc_refuse
        cmp     dword ptr [ebp+CBL_CUR],0
        jne     ldc_busy
ldc_take:
        mov     [ebp+CBL_CUR],esi
        mov     [ebp+CBL_CURBDD],edi
        cmp     byte ptr [ebp+CBL_VMWAIT],0
        je      ldc_lent
        call    ld_lend
ldc_lent:
        cmp     dword ptr [ebp+CBL_VMBUSY],0
        je      ldc_begin
        call    ld_vmcheck              ; over by now?
        jnc     ldc_begin
        mov     byte ptr [ebp+CBL_HELD],1
        TRC     68h             ; h
        call    ld_watchon
        jmp     ldc_out

ldc_begin:
        call    ld_go
ldc_out:
        sti
        popad
        ret

ldc_busy:
        cmp     dword ptr [ebp+CBL_CUR],-1
        je      ldc_choose
        mov     [ebp+CBL_PEND],esi
        mov     [ebp+CBL_PENDBDD],edi
        jmp     ldc_out
ldc_choose:
        test    byte ptr [esi+CB_FLAGS],BDCF_HIGH
        jz      ldc_older
        mov     eax,[ebp+CBL_PEND]
        test    byte ptr [eax+CB_FLAGS],BDCF_HIGH
        jz      ldc_take
ldc_older:
        xchg    esi,[ebp+CBL_PEND]
        xchg    edi,[ebp+CBL_PENDBDD]
        jmp     ldc_take

ldc_refuse:
        mov     word ptr [esi+CB_STATUS],BDS_BAD_CMD
        VXDCALL S_BD_COMMAND_COMPLETE
        jmp     ldc_out

;---------------------------------------------------------------------
; ld_begin - start the command in hand on the cable: ESI -> it, EDI ->
; its descriptor, EBP -> the cable, interrupts off.  EAX, ECX, EDX
; changed.
;---------------------------------------------------------------------
ld_begin:
IFDEF LRG_TRACE
        mov     al,'r'
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      lbg_trc
        mov     al,'w'
lbg_trc:
        call    ld_tchar
ENDIF
        mov     byte ptr [ebp+CBL_ACCUM],0
        mov     eax,dword ptr [esi+CB_SECTOR]
        mov     [ebp+CBL_NEXT],eax
        mov     eax,[esi+CB_BUFFER]
        test    byte ptr [esi+CB_FLAGS],BDCF_SG
        jz      lbg_flat
        mov     edx,[eax]               ; the first region's sectors
        mov     [ebp+CBL_REGLEFT],edx
        mov     ecx,[eax+4]
        mov     [ebp+CBL_PTR],ecx
        mov     [ebp+CBL_REGION],eax
lbg_sum:
        add     eax,8                   ; and the rest of the regions'
        mov     ecx,[eax]
        jecxz   lbg_counted
        add     edx,ecx
        jmp     lbg_sum
lbg_flat:
        mov     [ebp+CBL_PTR],eax
        mov     dword ptr [ebp+CBL_REGLEFT],0 ; counts down from 4G: never
                                        ; ends
        mov     edx,[esi+CB_COUNT]
lbg_counted:
        mov     [ebp+CBL_LEFT],edx
        or      edx,edx                 ; nothing to move: done (a count
        jz      lbg_nothing             ; of 0 is 256 to the drive)
        jmp     ld_start
lbg_nothing:
        mov     word ptr [esi+CB_STATUS],BDS_SUCCESS
        jmp     ld_done

;---------------------------------------------------------------------
; ld_start - give the controller the next piece of the command in
; hand: up to LD_MAXXFER sectors from CBL_NEXT.  EDI -> descriptor,
; ESI -> command, EBP -> the cable, interrupts off.  By PIO a read's
; sectors then arrive a block an interrupt, and a write's first block
; goes out here and the rest a block an interrupt - a block being one
; sector, or with block mode on the drive (BP_MULT) and more than one
; sector to move, that many; by DMA the engine moves them all and the
; drive interrupts once at the end.  CBL_GO is set last of all: until
; then the drive's status is not something the handler may act on.
; EAX, ECX, EDX changed.
;---------------------------------------------------------------------
ld_start:
        mov     byte ptr [ebp+CBL_GO],0
        cmp     byte ptr [ebp+CBL_HWVM],0 ; the controller holds what DOS
        je      lst_ours                ; programs left in it: kept first
        call    ld_snapshot
lst_ours:
        VXDCALL S_GET_TIME
        mov     [ebp+CBL_CMDTIME],eax
        mov     ecx,[ebp+CBL_LEFT]
        cmp     ecx,LD_MAXXFER
        jbe     lst_count
        mov     ecx,LD_MAXXFER
lst_count:
        mov     [ebp+CBL_BLOCK],cl
        mov     eax,[ebp+CBL_NEXT]
        add     [ebp+CBL_NEXT],ecx
        mov     byte ptr [ebp+CBL_EXT],0 ; past 28 bits: a 48-bit command
        cmp     byte ptr [edi+BP_MODE],2
        jne     lst_addr
        lea     edx,[eax+ecx]
        cmp     edx,LBA28_END
        jbe     lst_addr
        mov     byte ptr [ebp+CBL_EXT],1
lst_addr:
        call    ld_address

        mov     byte ptr [ebp+CBL_DMA],0 ; by the engine, if the drive is
        cmp     byte ptr [edi+BP_XFER],0 ; and the buffer can be
        je      lst_pio
        cmp     word ptr [ebp+CBL_BM],0
        je      lst_pio
        call    ld_prdbuild
        jc      lst_pio
        mov     byte ptr [ebp+CBL_DMA],1
lst_pio:
        mov     al,1                    ; by PIO, in blocks: the drive's
        cmp     byte ptr [ebp+CBL_DMA],0 ; size, for more than one sector
        jne     lst_mult
        cmp     byte ptr [ebp+CBL_BLOCK],2
        jb      lst_mult
        cmp     byte ptr [edi+BP_MULT],2
        jb      lst_mult
        mov     al,[edi+BP_MULT]
IFDEF LD_TESTLONG                       ; A rig's two lies about the size,
        add     al,al                   ; to see the nets hold: twice the
ENDIF                                   ; drive's, and ld_xfer must end each
IFDEF LD_TESTSHORT                      ; block where the drive does; half,
        shr     al,1                    ; and the half the drive still holds
ENDIF                                   ; out is found by ld_poll, or with
        STAT    SC_MULTCMD              ; LRGDiskPoll=off by ld_kick
lst_mult:
        mov     [ebp+CBL_MULT],al

        mov     al,[edi+BP_CONTROL]     ; interrupts on
        movzx   edx,word ptr [ebp+CBL_CTRL]
        out     dx,al
        IODELAY
        IODELAY
        mov     al,[ebp+CBL_TF+3]       ; the drive, before anything else
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_DRVHD
        out     dx,al
        call    ld_notbusy
        jc      lst_timeout
        cmp     byte ptr [ebp+CBL_RESET],0 ; after a DOS program's reset
        je      lst_set                 ; the drives need their parameters
        call    ld_reinit               ; again
        mov     al,[ebp+CBL_TF+3]
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_DRVHD
        out     dx,al
        call    ld_notbusy
        jc      lst_timeout
lst_set:
        cmp     byte ptr [ebp+CBL_DMA],0
        je      lst_regs
        call    ld_bmprep
lst_regs:
        movzx   edx,word ptr [ebp+CBL_DATA]
        inc     edx                     ; features
        cmp     byte ptr [ebp+CBL_EXT],0
        je      lst_low
        xor     al,al                   ; 48-bit: the high-order bytes
        out     dx,al                   ; first - a count's is 0, and an
        IODELAY                         ; LBA's past bit 31 are
        IODELAY
        inc     edx                     ; count
        out     dx,al
        IODELAY
        IODELAY
        inc     edx                     ; sector
        mov     al,[ebp+CBL_HOB]
        out     dx,al
        IODELAY
        IODELAY
        inc     edx                     ; cylinder low
        xor     al,al
        out     dx,al
        IODELAY
        IODELAY
        inc     edx                     ; cylinder high
        out     dx,al
        IODELAY
        IODELAY
        sub     edx,R_CYLHI-R_FEAT
lst_low:
        xor     al,al                   ; the features register: nothing
        out     dx,al                   ; (write precompensation to an
        IODELAY                         ; ST506; an ATA drive ignores it)
        IODELAY
        inc     edx                     ; count
        mov     al,[ebp+CBL_BLOCK]
        out     dx,al
        IODELAY
        IODELAY
        inc     edx                     ; sector
        mov     al,[ebp+CBL_TF]
        out     dx,al
        IODELAY
        IODELAY
        inc     edx                     ; cylinder low
        mov     al,[ebp+CBL_TF+1]
        out     dx,al
        IODELAY
        IODELAY
        inc     edx                     ; cylinder high
        mov     al,[ebp+CBL_TF+2]
        out     dx,al
        IODELAY
        IODELAY
        add     edx,R_CMD-R_CYLHI
        call    ld_cmdbyte              ; AL = the command
        out     dx,al
        cmp     byte ptr [ebp+CBL_DMA],0
        je      lst_piogo
        mov     byte ptr [ebp+CBL_GO],1
        jmp     ld_bmstart              ; the engine moves everything
lst_piogo:
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      lst_out
        IODELAY                         ; a write: the first block, once
        IODELAY                         ; the drive asks for it
        in      al,dx
        test    al,ST_DRQ
        jnz     lst_first
        VXDCALL S_GET_TIME
        mov     ecx,eax
lst_drq:
        sti
        IODELAY
        IODELAY
        in      al,dx
        cli
        test    al,ST_DRQ
        jnz     lst_first
        VXDCALL S_GET_TIME
        sub     eax,ecx
        cmp     eax,LD_DRQMS
        jb      lst_drq
        jmp     lst_timeout
lst_first:
        call    ld_xfer
lst_out:
        mov     byte ptr [ebp+CBL_GO],1
        ret

lst_timeout:
        TRC     13
        TRC     10
        TRC     54h             ; T
        TRCHEX  eax, 2
        TRC     20h
        TRCHEX  [ebp+CBL_NEXT], 8
        mov     word ptr [esi+CB_STATUS],BDS_DEVICE_ERR
        pushad
        call    ld_done
        popad
        ret

;---------------------------------------------------------------------
; ld_cmdbyte - AL = the command byte for the command in hand (ESI),
; by CBL_EXT, CBL_DMA and CBL_MULT: READ or WRITE SECTORS, or MULTIPLE,
; or DMA, or their EXT forms.  Nothing else changed.
;---------------------------------------------------------------------
ld_cmdbyte:
        cmp     byte ptr [ebp+CBL_DMA],0
        jne     lcb_dma
        cmp     byte ptr [ebp+CBL_MULT],1
        ja      lcb_mult
        mov     al,ATA_READ             ; 20h, 24h, 30h, 34h
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      lcb_ext
        mov     al,ATA_WRITE
lcb_ext:
        cmp     byte ptr [ebp+CBL_EXT],0
        je      lcb_out
        add     al,4
lcb_out:
        ret
lcb_mult:
        mov     al,ATA_READ_MULT        ; C4h, 29h, C5h, 39h
        cmp     byte ptr [ebp+CBL_EXT],0
        je      lcb_multw
        mov     al,ATA_READ_MULTX
lcb_multw:
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      lcb_out
        mov     al,ATA_WRITE_MULT
        cmp     byte ptr [ebp+CBL_EXT],0
        je      lcb_out
        mov     al,ATA_WRITE_MULTX
        ret
lcb_dma:
        mov     al,ATA_READ_DMA         ; C8h, 25h, CAh, 35h
        cmp     byte ptr [ebp+CBL_EXT],0
        je      lcb_dmaw
        mov     al,ATA_READ_DMAX
lcb_dmaw:
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      lcb_out
        mov     al,ATA_WRITE_DMA
        cmp     byte ptr [ebp+CBL_EXT],0
        je      lcb_out
        mov     al,ATA_WRITE_DMAX
        ret

;---------------------------------------------------------------------
; ld_address - EAX = sector number -> CBL_TF, the four registers'
; bytes, by the descriptor's mode, and CBL_HOB, bits 24-31 for a 48-bit
; command (CBL_EXT).  EDI -> descriptor.  EAX changed.
;---------------------------------------------------------------------
ld_address:
        push    ecx
        push    edx
        mov     cl,[edi+BP_UNIT]
        shl     cl,4
        cmp     byte ptr [edi+BP_MODE],0
        je      lad_chs
        mov     dword ptr [ebp+CBL_TF],eax ; bits 0-23, and 24-27 below
        shr     eax,24
        mov     [ebp+CBL_HOB],al
        and     byte ptr [ebp+CBL_TF+3],0Fh
        cmp     byte ptr [ebp+CBL_EXT],0
        je      lad_lba
        mov     byte ptr [ebp+CBL_TF+3],0 ; (48-bit: those four reserved)
lad_lba:
        or      cl,0E0h
        or      [ebp+CBL_TF+3],cl
        jmp     lad_out
lad_chs:
        xor     edx,edx
        div     dword ptr [edi+BD_SPT]
        inc     edx
        mov     [ebp+CBL_TF],dl         ; sectors count from 1
        xor     edx,edx
        div     dword ptr [edi+BD_HEADS]
        mov     word ptr [ebp+CBL_TF+1],ax ; the cylinder
        or      cl,dl                   ; the head
        or      cl,0A0h
        mov     [ebp+CBL_TF+3],cl
lad_out:
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; ld_notbusy - wait up to LD_BUSYMS for the selected drive's BSY to go
; (LD_RESETMS after a DOS program's reset), letting interrupts in while
; it waits.  CF if it does not.  AL = status; nothing else changed.
;---------------------------------------------------------------------
ld_notbusy:
        push    ecx
        push    edx
        movzx   edx,word ptr [ebp+CBL_CTRL]
        IODELAY
        IODELAY
        in      al,dx
        test    al,ST_BSY
        jz      lnb_ok
        push    eax
        VXDCALL S_GET_TIME
        mov     ecx,eax
        pop     eax
lnb_wait:
        sti
        IODELAY
        IODELAY
        in      al,dx
        cli
        test    al,ST_BSY
        jz      lnb_ok
        push    eax
        VXDCALL S_GET_TIME
        sub     eax,ecx
        cmp     eax,LD_BUSYMS
        jb      lnb_again
        cmp     byte ptr [ebp+CBL_RESET],0
        je      lnb_late
        cmp     eax,LD_RESETMS
        jb      lnb_again
lnb_late:
        pop     eax
        stc
        jmp     lnb_out
lnb_again:
        pop     eax
        jmp     lnb_wait
lnb_ok:
        clc
lnb_out:
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; ld_reinit - a DOS program reset the cable (CBL_RESET): each of
; LRGDISK's drives on it is given its parameters again, the way the
; BIOS and the kernel gave them at boot - a drive used by CHS its
; geometry (INITIALIZE DEVICE PARAMETERS), a drive used by DMA its
; transfer mode (SET FEATURES), a drive used in blocks the block's
; size (SET MULTIPLE MODE) - polled, with its interrupt off.
; EBP -> the cable, interrupts off.  EAX, ECX, EDX changed.
;---------------------------------------------------------------------
ld_reinit:
        push    edi
        TRC     52h             ; R
        mov     edi,OFFSET FLAT:ld_bdd
lri_unit:
        cmp     byte ptr [edi+BD_INT13],0
        je      lri_next
        cmp     [edi+BP_CABLE],ebp
        jne     lri_next
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,CTL_NIEN OR 08h
        out     dx,al
        IODELAY
        IODELAY
        mov     al,[edi+BP_UNIT]
        shl     al,4
        or      al,0A0h
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_DRVHD
        out     dx,al
        call    ld_notbusy
        jc      lri_done
        cmp     byte ptr [edi+BP_MODE],0
        jne     lri_xfer
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_COUNT             ; sectors a track
        mov     al,[edi+BP_ISPT]
        out     dx,al
        IODELAY
        IODELAY
        add     edx,R_DRVHD-R_COUNT     ; and the last head
        mov     al,[edi+BP_IHEADS]
        and     al,0Fh
        mov     ah,[edi+BP_UNIT]
        shl     ah,4
        or      al,ah
        or      al,0A0h
        out     dx,al
        IODELAY
        IODELAY
        inc     edx
        mov     al,ATA_INITPARM
        out     dx,al
        call    ld_delay
        call    ld_notbusy
lri_xfer:
        cmp     byte ptr [edi+BP_XFER],0
        je      lri_mult
        movzx   edx,word ptr [ebp+CBL_DATA]
        inc     edx                     ; features: the transfer mode
        mov     al,SF_XFERMODE
        out     dx,al
        IODELAY
        IODELAY
        inc     edx                     ; count: which
        mov     al,[edi+BP_XFER]
        out     dx,al
        IODELAY
        IODELAY
        add     edx,R_CMD-R_COUNT
        mov     al,ATA_SETFEAT
        out     dx,al
        call    ld_delay
        call    ld_notbusy
lri_mult:
        cmp     byte ptr [edi+BP_MULT],2 ; and the block's size; a drive
        jb      lri_done                ; that will not have it back is one
        movzx   edx,word ptr [ebp+CBL_DATA] ; sector a block from now on
        add     edx,R_COUNT
        mov     al,[edi+BP_MULT]
        out     dx,al
        IODELAY
        IODELAY
        add     edx,R_CMD-R_COUNT
        mov     al,ATA_SETMULT
        out     dx,al
        call    ld_delay
        call    ld_notbusy
        jc      lri_nomult
        test    al,ST_ERR
        jz      lri_done
lri_nomult:
        mov     byte ptr [edi+BP_MULT],0
lri_done:
        movzx   edx,word ptr [ebp+CBL_DATA] ; nothing left pending, and
        add     edx,R_STATUS            ; the interrupt back on
        in      al,dx
        mov     al,[edi+BP_CONTROL]
        movzx   edx,word ptr [ebp+CBL_CTRL]
        out     dx,al
lri_next:
        add     edi,BP_SIZE
        cmp     edi,OFFSET FLAT:ld_bdd+4*BP_SIZE
        jb      lri_unit
        mov     byte ptr [ebp+CBL_RESET],0
        pop     edi
        ret

;---------------------------------------------------------------------
; ld_wrsec - one sector out of the buffer to the controller, a
; doubleword at a time if the drive's data port has been proved that
; way (BP_IO32), and the counts and the pointer moved on.  ECX, EDX
; changed.
;---------------------------------------------------------------------
ld_wrsec:
        push    esi
        mov     edx,[ebp+CBL_CURBDD]
        mov     cl,[edx+BP_IO32]
        mov     esi,[ebp+CBL_PTR]
        movzx   edx,word ptr [ebp+CBL_DATA]
        cld
        or      cl,cl
        jz      lws_words
        mov     ecx,128
        rep     outsd
        STAT    SC_IO32
        jmp     lws_written
lws_words:
        mov     ecx,256
        rep     outsw
lws_written:
        dec     dword ptr [ebp+CBL_REGLEFT]
        jnz     lws_same
        mov     esi,[ebp+CBL_REGION]    ; the end of a region: the next
        add     esi,8
        mov     ecx,[esi]
        jecxz   lws_same                ; (none: the command is ending)
        mov     [ebp+CBL_REGLEFT],ecx
        mov     [ebp+CBL_REGION],esi
        mov     esi,[esi+4]
lws_same:
        mov     [ebp+CBL_PTR],esi
        dec     dword ptr [ebp+CBL_LEFT]
        dec     byte ptr [ebp+CBL_BLOCK]
        pop     esi
        ret

;---------------------------------------------------------------------
; ld_rdsec - one sector from the controller into the buffer, a
; doubleword at a time if the drive's data port has been proved that
; way (BP_IO32), and the counts and the pointer moved on.  ECX, EDX
; changed.
;---------------------------------------------------------------------
ld_rdsec:
        push    edi
        mov     edx,[ebp+CBL_CURBDD]
        mov     cl,[edx+BP_IO32]
        mov     edi,[ebp+CBL_PTR]
        movzx   edx,word ptr [ebp+CBL_DATA]
        cld
        or      cl,cl
        jz      lrd_words
        mov     ecx,128
        rep     insd
        STAT    SC_IO32
        jmp     lrd_read
lrd_words:
        mov     ecx,256
        rep     insw
lrd_read:
        dec     dword ptr [ebp+CBL_REGLEFT]
        jnz     lrd_same
        mov     edi,[ebp+CBL_REGION]    ; the end of a region: the next
        add     edi,8
        mov     ecx,[edi]
        jecxz   lrd_same                ; (none: the command is ending)
        mov     [ebp+CBL_REGLEFT],ecx
        mov     [ebp+CBL_REGION],edi
        mov     edi,[edi+4]
lrd_same:
        mov     [ebp+CBL_PTR],edi
        dec     dword ptr [ebp+CBL_LEFT]
        dec     byte ptr [ebp+CBL_BLOCK]
        pop     edi
        ret

;---------------------------------------------------------------------
; ld_xfer - one block moved, the way the command in hand goes: the
; drive has DRQ up for CBL_MULT sectors, or for what is left of the
; controller command if that is fewer.  Between its sectors other
; interrupts are let in for a moment - the drive raises none of its
; own with a block half moved, and CBL_INXFER turns away one left over
; from before - so that a block keeps nothing waiting longer than a
; sector does.
;
; THE BLOCK'S SIZE IS NOT TAKEN ON TRUST.  After the first, a sector
; is moved only if the drive still has DRQ up for it, with no error:
; a drive whose blocks turned out shorter than the size it was set to
; would otherwise be read past what it had, and what comes then is
; not on the disk.  The block ends where the drive ends it, and the
; rest of the controller command comes as the drive offers it.
;
; ESI -> the command, EBP -> the cable; interrupts off, and off again
; on return.  ECX, EDX changed.
;---------------------------------------------------------------------
ld_xfer:
        push    eax
        mov     ah,[ebp+CBL_MULT]
        cmp     ah,[ebp+CBL_BLOCK]
        jbe     lxf_sized
        mov     ah,[ebp+CBL_BLOCK]
lxf_sized:
        mov     byte ptr [ebp+CBL_INXFER],1
        STAT    SC_BLOCK
lxf_sector:
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        jne     lxf_write
        call    ld_rdsec
        jmp     lxf_moved
lxf_write:
        call    ld_wrsec
lxf_moved:
        STAT    SC_SECTOR
        inc     byte ptr [ebp+CBL_BURST]
        dec     ah
        jz      lxf_done
        sti
        nop
        cli
        call    ld_delay                ; (400ns: the status is the
        movzx   edx,word ptr [ebp+CBL_CTRL] ; drive's answer by now)
        in      al,dx
        and     al,ST_BSY OR ST_DRQ OR ST_ERR
        cmp     al,ST_DRQ
        je      lxf_sector
        STAT    SC_SHORT
lxf_done:
        mov     byte ptr [ebp+CBL_INXFER],0
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_poll - a block has just been moved and the drive owes another
; interrupt, for the next block or for the end of a write: is it ready
; already?  LD_POLLN looks at the alternate status, which leaves the
; request alone; if BSY goes, the status itself is read - that drops
; the request, with interrupts off, before the processor has seen it -
; and comes back in CL.  CF if the drive is still busy, or if no look
; is to be taken (LRGDiskPoll=off, or LD_BURST sectors moved for this
; interrupt already): the interrupt will say.
;
; Other interrupts are let in for a moment first.  The drive's own may
; be among them, if the block is ready that soon - turned away by
; CBL_INXFER, and then the look that follows finds the drive ready, so
; nothing is left waiting for an interrupt that has been and gone.
; From there to the return interrupts stay off: a request raised in
; that time is still there when they come back on.
;
; EBP -> the cable; interrupts off, and off again on return.  EAX,
; ECX, EDX changed.
;---------------------------------------------------------------------
ld_poll:
        cmp     byte ptr [ld_pollon],0
        je      lpo_no
        cmp     byte ptr [ebp+CBL_BURST],LD_BURST
        jae     lpo_no
        call    ld_window
        call    ld_delay                ; (the status is not the drive's
        movzx   edx,word ptr [ebp+CBL_CTRL] ; answer for 400ns)
        mov     ecx,LD_POLLN
lpo_look:
        in      al,dx
        test    al,ST_BSY
        jz      lpo_ready
        dec     ecx
        jnz     lpo_look
        STAT    SC_WAITED
lpo_no:
        stc
        ret
lpo_ready:
        STAT    SC_POLLED
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_STATUS
        in      al,dx
        mov     cl,al
        clc
        ret

;---------------------------------------------------------------------
; ld_window - other interrupts let in for a moment, CBL_INXFER saying
; so: one of the cable's own that arrives is ended and nothing else
; (ldh_within).  EBP -> the cable; interrupts off, and off again on
; return.  Nothing changed.
;---------------------------------------------------------------------
ld_window:
        mov     byte ptr [ebp+CBL_INXFER],1
        sti
        nop
        cli
        mov     byte ptr [ebp+CBL_INXFER],0
        ret

;---------------------------------------------------------------------
; ld_drain - a read has ended in an error with the drive still holding
; data out: taken and dropped, so that the controller is not left in
; the middle of a block.  EBP -> the cable.  Nothing changed.
;---------------------------------------------------------------------
ld_drain:
        push    eax
        push    ecx
        push    edx
        mov     ecx,256*256             ; no command has more words left
ldr_look:
        movzx   edx,word ptr [ebp+CBL_CTRL]
        in      al,dx
        test    al,ST_BSY
        jnz     ldr_out
        test    al,ST_DRQ
        jz      ldr_out
        movzx   edx,word ptr [ebp+CBL_DATA]
        in      ax,dx
        dec     ecx
        jnz     ldr_look
ldr_out:
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_delay - 400ns and more: four reads of the alternate status.  A
; drive may take that long to raise BSY after a command or a block.
; EBP -> the cable.  Nothing changed.
;---------------------------------------------------------------------
ld_delay:
        push    eax
        push    edx
        movzx   edx,word ptr [ebp+CBL_CTRL]
        in      al,dx
        in      al,dx
        in      al,dx
        in      al,dx
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_prdbuild - the engine's table for the controller command about to
; go: CBL_BLOCK sectors from where the command's buffer has got to,
; region by region and page by page, each entry a run of physical
; addresses that does not cross a 64K boundary, and the buffer's
; counts and pointer moved past them (the data moves without LRGDISK's
; help).  CF if it cannot be done - an odd address, a page not present,
; more entries than the table holds - with the counts and pointer as
; they were, for PIO.  EBP -> the cable, interrupts off.  EAX, ECX,
; EDX changed.
;
; EACH PAGE IS WRITTEN BY THE PROCESSOR ON THE WAY, a byte read and put
; back.  Hardware does not need it: a bus master's write reaches the
; same memory the processor reads.  An emulator that translates the
; guest's code and keeps the translation (QEMU's TCG) throws it away
; when the PROCESSOR writes the page, and not when a device does - so
; a program loaded by the engine into a page that once held code ran
; as the old code.  Windows loads KRNL386.EXE into memory that has run
; other programs, and did exactly that.  A write of its own to each
; page first costs nothing anyone will notice.
;---------------------------------------------------------------------
ld_prdbuild:
        push    ebx
        push    esi
        push    edi
        push    dword ptr [ebp+CBL_PTR] ; kept, for a failure
        push    dword ptr [ebp+CBL_LEFT]
        push    dword ptr [ebp+CBL_REGLEFT]
        push    dword ptr [ebp+CBL_REGION]
        mov     edi,[ebp+CBL_PRD]       ; EDI -> the next entry
        xor     ebx,ebx                 ; EBX = entries so far
        movzx   esi,byte ptr [ebp+CBL_BLOCK] ; ESI = sectors to describe
lpb_piece:
        mov     ecx,[ebp+CBL_REGLEFT]   ; a piece: what is left of the
        or      ecx,ecx                 ; region, or of the block
        jz      lpb_whole
        cmp     ecx,esi
        jbe     lpb_sized
lpb_whole:
        mov     ecx,esi
lpb_sized:
        push    ecx                     ; the piece's sectors
        shl     ecx,9                   ; ECX = its bytes
        mov     eax,[ebp+CBL_PTR]
        test    al,1
        jnz     lpb_fail1
        mov     edx,eax                 ; its pages' table entries
        and     edx,0FFFh
        add     edx,ecx
        add     edx,0FFFh
        shr     edx,12                  ; EDX = pages (17 at most)
        shr     eax,12                  ; EAX = the first
        push    ecx
        push    0
        push    OFFSET FLAT:ld_ptes
        push    edx
        push    eax
        VXDCALL S_COPYPAGETABLE
        add     esp,16
        pop     ecx
        or      eax,eax
        jz      lpb_fail1
        mov     eax,[ebp+CBL_PTR]       ; EAX = where the piece has got
        mov     edx,OFFSET FLAT:ld_ptes ; to, ECX = bytes left of it
lpb_page:
        push    edx
        mov     dl,[eax]                ; (written by the processor: see
        mov     [eax],dl                ; above)
        pop     edx
        push    edx
        mov     edx,[edx]               ; the page's entry
        test    dl,P_PRES
        jz      lpb_fail2
        and     edx,0FFFFF000h
        push    eax
        and     eax,0FFFh
        or      edx,eax                 ; EDX = the bytes' physical start
        neg     eax
        add     eax,1000h               ; EAX = bytes to the page's end
        cmp     eax,ecx
        jbe     lpb_len
        mov     eax,ecx
lpb_len:
        xchg    eax,edx                 ; EAX = start, EDX = length
        call    ld_prdadd
        jc      lpb_fail3
        pop     eax
        add     eax,edx
        sub     ecx,edx
        pop     edx
        add     edx,4
        or      ecx,ecx
        jnz     lpb_page

        pop     ecx                     ; the piece is in the table: the
        sub     esi,ecx                 ; counts and the pointer past it
        sub     [ebp+CBL_LEFT],ecx
        mov     eax,ecx
        shl     eax,9
        add     [ebp+CBL_PTR],eax
        cmp     dword ptr [ebp+CBL_REGLEFT],0
        je      lpb_more                ; (one region: never ends)
        sub     [ebp+CBL_REGLEFT],ecx
        jnz     lpb_more
        mov     eax,[ebp+CBL_REGION]    ; the end of a region: the next
        add     eax,8
        mov     ecx,[eax]
        jecxz   lpb_more                ; (none: the command is ending)
        mov     [ebp+CBL_REGLEFT],ecx
        mov     [ebp+CBL_REGION],eax
        mov     eax,[eax+4]
        mov     [ebp+CBL_PTR],eax
lpb_more:
        or      esi,esi
        jnz     lpb_piece
        or      word ptr [edi-PRD_LEN+PRD_COUNT+2],PRD_EOT
        add     esp,16                  ; (the kept state, not needed)
        pop     edi
        pop     esi
        pop     ebx
        clc
        ret

lpb_fail3:
        pop     eax
lpb_fail2:
        pop     edx
lpb_fail1:
        pop     ecx
        pop     dword ptr [ebp+CBL_REGION]
        pop     dword ptr [ebp+CBL_REGLEFT]
        pop     dword ptr [ebp+CBL_LEFT]
        pop     dword ptr [ebp+CBL_PTR]
        pop     edi
        pop     esi
        pop     ebx
        stc
        ret

; ld_prdadd - EDX bytes at physical EAX into the table: onto the last
; entry if they follow it within the same 64K, else an entry of their
; own.  EBX = entries, EDI -> the next.  CF if the table is full.  EAX
; and EDX kept.
ld_prdadd:
        push    ecx
        or      ebx,ebx
        jz      lpa_new
        movzx   ecx,word ptr [edi-PRD_LEN+PRD_COUNT]
        jecxz   lpa_new                 ; (0: a full 64K already)
        add     ecx,[edi-PRD_LEN+PRD_ADDR] ; where the last entry ends
        cmp     ecx,eax
        jne     lpa_new
        lea     ecx,[eax+edx-1]         ; the new last byte: in the
        xor     ecx,[edi-PRD_LEN+PRD_ADDR] ; entry's 64K?
        test    ecx,0FFFF0000h
        jnz     lpa_new
        add     [edi-PRD_LEN+PRD_COUNT],dx ; (10000h stores as 0: 64K)
        pop     ecx
        clc
        ret
lpa_new:
        cmp     ebx,LD_PRDMAX
        jae     lpa_full
        mov     [edi+PRD_ADDR],eax
        mov     [edi+PRD_COUNT],dx
        mov     word ptr [edi+PRD_COUNT+2],0
        add     edi,PRD_LEN
        inc     ebx
        pop     ecx
        clc
        ret
lpa_full:
        pop     ecx
        stc
        ret

;---------------------------------------------------------------------
; ld_bmprep - the engine made ready for the command about to go:
; stopped, its latched bits cleared, pointed at the table, told the
; direction.  Started only once the drive has the command (ld_bmstart)
; - the engine moves data the moment the drive asks, and must not be
; waiting for a command that is not there yet.  ESI -> the command,
; EBP -> the cable.  EAX, EDX changed.
;---------------------------------------------------------------------
ld_bmprep:
        movzx   edx,word ptr [ebp+CBL_BM]
        xor     al,al
        out     dx,al                   ; stop
        add     edx,BM_STATUS
        in      al,dx
        or      al,BM_ST_IRQ OR BM_ST_ERR
        out     dx,al                   ; both write-one-to-clear
        add     edx,BM_PRDT-BM_STATUS
        mov     eax,[ebp+CBL_PRDPHYS]
        out     dx,eax
        sub     edx,BM_PRDT
        mov     al,BM_CM_READ           ; a read: the engine writes memory
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      lbp_dir
        xor     al,al
lbp_dir:
        out     dx,al
        ret

ld_bmstart:
        movzx   edx,word ptr [ebp+CBL_BM]
        in      al,dx
        or      al,BM_CM_START
        out     dx,al
        TRC     64h             ; d
        ret

;---------------------------------------------------------------------
; ld_hwint - BDD_Hw_Int_Proc, called by BlockDev for IRQ 14: EAX = the
; IRQ's handle, EDI -> a descriptor, interrupts off.  CF clear if the
; interrupt was dealt with - the drive's, for LRGDISK's command, or a
; DOS program's device's, sent on to the VM whose command is on the
; cable (BlockDev's own EOI procedure gives the controller its EOI
; when the VM has had the interrupt).  CF set leaves it to BlockDev.
; Also the second cable's, through ld_hwint15.  EBP kept.
;
; ld_cblint is the work, for either cable (EBP -> it) and for ld_kick,
; which has no interrupt to end (EAX = 0).  What it does it reads off
; the drive, not off the interrupt - see the top of the file: an
; interrupt may be one a block taken by ld_poll left behind.  The
; request is dropped (the status read) and the interrupt ended before
; any data moves, as WDCTRL does it; from there a block at a time
; (ld_xfer) while the drive keeps up (ld_poll), and the controller
; command's end.
;---------------------------------------------------------------------
ld_hwint:
        push    ebp
        mov     ebp,[edi+BP_CABLE]
        call    ld_cblint
        pop     ebp
        ret

ld_cblint:
        cmp     dword ptr [ebp+CBL_VMBUSY],0 ; a DOS program's command is
        jne     ldh_theirs              ; on the cable: theirs
        cmp     edi,[ebp+CBL_CURBDD]
        jne     ldh_no
        mov     ecx,[ebp+CBL_CUR]
        test    ecx,ecx
        jz      ldh_no
        inc     ecx                     ; -1: between commands
        jz      ldh_no
        cmp     byte ptr [ebp+CBL_INXFER],0
        jne     ldh_within
        cmp     byte ptr [ebp+CBL_GO],0 ; nothing with the controller that
        je      ldh_no                  ; the drive's status would be about
        mov     ecx,eax
        movzx   edx,word ptr [ebp+CBL_CTRL]
        in      al,dx
        test    al,ST_BSY
        jz      ldh_mine
        TRC     62h             ; b
        mov     eax,ecx
        jmp     ldh_stale
ldh_mine:
        cmp     byte ptr [ebp+CBL_DMA],0
        je      ldh_status
        movzx   edx,word ptr [ebp+CBL_BM] ; the engine's account of it:
        add     edx,BM_STATUS           ; has the drive interrupted at all
        in      al,dx                   ; since the command went?  Not the
        test    al,BM_ST_IRQ            ; command's end if not
        jnz     ldh_engine
        mov     eax,ecx
        jmp     ldh_stale
ldh_engine:
        mov     ah,al                   ; ...then stopped and cleared
        sub     edx,BM_STATUS
        xor     al,al
        out     dx,al
        add     edx,BM_STATUS
        in      al,dx
        or      al,BM_ST_IRQ OR BM_ST_ERR
        out     dx,al
        mov     [ebp+CBL_BMST],ah
ldh_status:
        IODELAY
        IODELAY
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_STATUS            ; and now the request is gone
        in      al,dx
        xchg    ecx,eax                 ; CL = status, EAX = the IRQ
        call    ld_eoi
        STAT    SC_INT
        mov     byte ptr [ebp+CBL_BURST],0
        mov     esi,[ebp+CBL_CUR]

        ; CL = the status, read from the status register: what the
        ; drive wants now, whichever interrupt this was
ldh_have:
        test    cl,ST_ERR
        jnz     ldh_error
        or      [ebp+CBL_ACCUM],cl
        cmp     byte ptr [ebp+CBL_DMA],0
        je      ldh_pio

        ; by the engine: the whole controller command moved, unless
        ; the engine says otherwise - an error of its own, or a table
        ; it had not finished when the drive stopped - in which case
        ; the drive goes back to PIO for good and the command is tried
        ; again
        mov     ch,[ebp+CBL_BMST]
        test    ch,BM_ST_ERR OR BM_ST_ACTIVE
        jz      ldh_moved
        TRC     13
        TRC     10
        TRC     44h             ; D
        movzx   eax,ch
        TRCHEX  eax, 2
        TRC     13
        TRC     10
        mov     edi,[ebp+CBL_CURBDD]
        mov     byte ptr [edi+BP_XFER],0
        jmp     ldh_retry
ldh_moved:
        mov     byte ptr [ebp+CBL_BLOCK],0
        jmp     ldh_ended

        ; by PIO: a block wanted, if DRQ is up
ldh_pio:
        test    cl,ST_DRQ
        jz      ldh_nodrq
        cmp     byte ptr [ebp+CBL_BLOCK],0 ; (wanted with none left to
        je      ldh_retry               ; move: not the command that went)
        call    ld_xfer
        cmp     byte ptr [ebp+CBL_BLOCK],0
        je      ldh_last
        call    ld_poll                 ; the next block: ready already,
        jc      ldh_eat                 ; or its interrupt will say
        jmp     ldh_have

        ; the controller command's last block is moved.  A read is
        ; over with that; a write is over when the drive says the
        ; block is written, which is one more interrupt - or one more
        ; look.
ldh_last:
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      ldh_ended
        call    ld_poll
        jc      ldh_eat
        jmp     ldh_have

        ; neither busy nor wanting data: the end of a write whose
        ; blocks have all gone; anything else is an interrupt from
        ; before, with nothing to do
ldh_nodrq:
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        je      ldh_eat
        cmp     byte ptr [ebp+CBL_BLOCK],0
        jne     ldh_eat

        ; The controller command is over: the next piece of the
        ; command in hand, or its end.  Other interrupts are let in
        ; first, and with them what a block taken by looking may have
        ; left in the interrupt controller - turned away here, where
        ; nothing is on the cable, and not in the middle of the next
        ; command.
ldh_ended:
        call    ld_window
        mov     byte ptr [ebp+CBL_GO],0
        mov     edi,[ebp+CBL_CURBDD]
        cmp     dword ptr [ebp+CBL_LEFT],0
        je      ldh_complete
        call    ld_start
        jmp     ldh_eat

ldh_complete:
        mov     esi,[ebp+CBL_CUR]
        xor     eax,eax                 ; BDS_SUCCESS
        cmp     [ebp+CBL_RETRIES],al
        je      ldh_ecc
        mov     al,BDS_RETRIED
        jmp     ldh_status2
ldh_ecc:
        test    byte ptr [ebp+CBL_ACCUM],ST_CORR
        jz      ldh_status2
        mov     al,BDS_CORRECTED
ldh_status2:
        mov     [esi+CB_STATUS],ax
IFDEF LRG_TRACE
        or      ax,ax
        jz      ldh_trcok
        TRC     53h             ; S
        TRCHEX  eax, 2
ldh_trcok:
ENDIF
        call    ld_done
ldh_eat:
        clc
        ret
ldh_no:
        stc
        ret

        ; An interrupt let in by LRGDISK itself (ld_window), while a
        ; block was being moved or just after one: the code that let
        ; it in goes by the drive's status next, whatever this was.
        ; The request is dropped and the interrupt ended, no more.
ldh_within:
        STAT    SC_WITHIN
        push    eax
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_STATUS
        in      al,dx
        pop     eax
        call    ld_eoi
        clc
        ret

        ; An interrupt with a command on the cable and nothing for it
        ; to do - the drive still busy, or the engine saying the drive
        ; has not interrupted: one from before.  Ended, and no more.
ldh_stale:
        STAT    SC_STALE
        call    ld_eoi
        clc
        ret

        ; a DOS program's device interrupted: on to its VM, whose
        ; handler - the BIOS's, or a driver's that hooked the IRQ -
        ; reads the status and ends it, as it would with no 32-bit disk
        ; access at all
ldh_theirs:
        TRC     69h             ; i
        push    ebx
        mov     ebx,[ebp+CBL_VMBUSY]
        VXDCALL S_VPICD_SET_INT_REQ
        pop     ebx
        clc
        ret

; An error: status in CL.  A read with data waiting has it taken and
; dropped, so the controller is not left in the middle of a block;
; then the whole command again, LD_MAXTRIES times - a sector a block,
; from now on, if it was a block-mode command that went wrong.
ldh_error:
IFDEF LRG_TRACE
        TRC     13
        TRC     10
        TRC     45h             ; E
        movzx   eax,cl
        TRCHEX  eax, 2
        movzx   edx,word ptr [ebp+CBL_DATA]
        inc     edx                     ; the error register
        in      al,dx
        TRCHEX  eax, 2
        TRC     20h
        TRCHEX  [ebp+CBL_NEXT], 8
        TRC     20h
        movzx   eax,byte ptr [ebp+CBL_BLOCK]
        TRCHEX  eax, 2
        TRC     13
        TRC     10
ENDIF
        cmp     byte ptr [ebp+CBL_DMA],0
        jne     ldh_retry
        cmp     word ptr [esi+CB_COMMAND],BDC_READ
        jne     ldh_retry
        call    ld_drain
ldh_retry:
        mov     byte ptr [ebp+CBL_GO],0
        mov     edi,[ebp+CBL_CURBDD]
        cmp     byte ptr [ebp+CBL_MULT],1
        jbe     ldh_tries
        mov     byte ptr [edi+BP_MULT],0
ldh_tries:
        inc     byte ptr [ebp+CBL_RETRIES]
        cmp     byte ptr [ebp+CBL_RETRIES],LD_MAXTRIES
        ja      ldh_fail
        mov     esi,[ebp+CBL_CUR]
        mov     dword ptr [ebp+CBL_CUR],0
        call    ld_command
        jmp     ldh_eat
ldh_fail:
        TRC     46h             ; F
        mov     esi,[ebp+CBL_CUR]
        mov     word ptr [esi+CB_STATUS],BDS_MEDIA_ERR
        call    ld_done
        jmp     ldh_eat

; ld_eoi - the interrupt whose handle is in EAX ended at the interrupt
; controller; none if EAX is 0 (ld_kick: there was no interrupt).
; Nothing changed.
ld_eoi:
        or      eax,eax
        jz      leo_out
        VXDCALL S_VPICD_PHYS_EOI
leo_out:
        ret

;---------------------------------------------------------------------
; ld_kick - a command the controller has had for LD_LOSTMS (ld_tick):
; what the handler would do is done now, as if the interrupt had come.
; It does nothing while the drive is busy, so a command that is merely
; slow is left alone; one whose interrupt was lost goes on from here.
; EBP -> the cable; interrupts off.  Nothing changed.
;---------------------------------------------------------------------
ld_kick:
        pushad
        cmp     dword ptr [ebp+CBL_VMBUSY],0
        jne     lkk_out
        cmp     byte ptr [ebp+CBL_GO],0
        je      lkk_out
        TRC     4Bh             ; K
        STAT    SC_KICK
        mov     edi,[ebp+CBL_CURBDD]
        xor     eax,eax
        call    ld_cblint
lkk_out:
        popad
        ret

;---------------------------------------------------------------------
; ld_hwint15 - VID_Hw_Int_Proc for IRQ 15, the second cable's, which
; LRGDISK has from VPICD: EAX = the handle, EBX = the current VM.  As
; ld_hwint; an interrupt that is nobody's is ended here.  ld_eoi15 -
; VID_EOI_Proc: a VM has had the interrupt sent on to it and ended it.
;---------------------------------------------------------------------
ld_hwint15:
        pushad
        mov     ebp,OFFSET FLAT:ld_cbl1
        mov     edi,[ebp+CBL_CURBDD]
        call    ld_cblint
        jnc     lh15_out
        TRC     3Fh             ; ?
        VXDCALL S_VPICD_PHYS_EOI
lh15_out:
        popad
        ret

ld_eoi15:
        push    eax
        VXDCALL S_VPICD_CLEAR_INT_REQ
        pop     eax
        VXDCALL S_VPICD_PHYS_EOI
        ret

;---------------------------------------------------------------------
; ld_done - the command in hand has its status: tell BlockDev, which
; turns interrupts on and may send another command from inside the
; call, then start the one waiting, if any.  EDI -> its descriptor,
; EBP -> the cable.
;---------------------------------------------------------------------
ld_done:
        xor     esi,esi
        mov     [ebp+CBL_CMDTIME],esi
        mov     byte ptr [ebp+CBL_GO],0
        cmp     [ebp+CBL_PEND],esi
        je      ldd_finish
        or      esi,-1
ldd_finish:
        xchg    esi,[ebp+CBL_CUR]
        mov     byte ptr [ebp+CBL_RETRIES],0
        push    ebp
        VXDCALL S_BD_COMMAND_COMPLETE
        pop     ebp
        cli
        cmp     byte ptr [ebp+CBL_VMWAIT],0 ; a DOS program's command
        je      ldd_ours                ; waiting for the cable goes first
        cmp     dword ptr [ebp+CBL_CUR],0 ; (unless BlockDev's call has
        je      ldd_lend                ; started one of LRGDISK's)
        cmp     dword ptr [ebp+CBL_CUR],-1
        jne     ldd_ours
ldd_lend:
        call    ld_lend
ldd_ours:
        call    ld_xrelease
        cmp     dword ptr [ebp+CBL_CUR],-1
        jne     ldd_out
        xor     esi,esi
        mov     [ebp+CBL_CUR],esi
        xchg    esi,[ebp+CBL_PEND]
        mov     edi,[ebp+CBL_PENDBDD]
        jmp     ld_command
ldd_out:
        ret

;---------------------------------------------------------------------
; ld_xlate - INT13.386's Int13_Translate_VM_Int, hooked: every INT 13h
; a VM makes passes here on its way to the BIOS, and INT13.386 turns
; the ones for a drive with a 32-bit driver into BlockDev commands -
; for BIOS disks 80h and 81h, and from a table that stops at AH=1Ah:
; the extensions came after it, so for such a drive AH=42h fails, and
; with it every sector past what CHS can name: a FAT32 volume over
; 8GB, or one that starts there.  For a drive of LRGDISK's this
; answers the extensions itself - AH=41h, 42h, 43h, 44h, 47h and 48h
; - through BlockDev, and for one numbered 82h or 83h, which INT13.386
; leaves to the BIOS, the rest of INT 13h as INT13.386 would; and
; hands everything else on.  EBX = the VM, EBP -> its registers.  CF
; clear: answered, and the VM's INT 13h returned from (Simulate_Iret,
; as INT13.386 does); CF set: on to the BIOS.  EBX and EBP kept.
;---------------------------------------------------------------------
ld_xlate:
        cmp     byte ptr [ld_rawbios],0 ; LRGDISK's own question to the
        jne     lx_bios                 ; BIOS: past INT13.386, to the VM's
        mov     al,[ebp+CL_DL]          ; one of LRGDISK's?  (an unused
        cmp     al,80h                  ; descriptor's BIOS disk is 0)
        jb      lx_on
        push    edi
        mov     edi,OFFSET FLAT:ld_bdd
lx_find:
        cmp     al,[edi+BD_INT13]
        je      lx_found
        add     edi,BP_SIZE
        cmp     edi,OFFSET FLAT:ld_bdd+4*BP_SIZE
        jb      lx_find
        pop     edi
lx_on:
        jmp     dword ptr [ld_prev13]
lx_bios:
        stc
        ret

lx_found:
        cmp     al,82h                  ; 80h and 81h: INT13.386's, but
        jae     lx_all                  ; for the extensions
        cmp     byte ptr [ebp+CL_AH],41h
        jb      lx_onpop
        cmp     byte ptr [ebp+CL_AH],48h
        ja      lx_onpop
lx_all:
        VXDCALL S_SIMULATE_IRET
        mov     al,[ebp+CL_AH]
        cmp     al,41h
        je      lx_41
        cmp     al,42h
        je      lx_rw
        cmp     al,43h
        je      lx_rw
        cmp     al,48h
        je      lx_48
        cmp     al,44h                  ; verify and seek: nothing to do
        je      lx_ok
        cmp     al,47h
        je      lx_ok
        cmp     al,40h
        jae     lx_badfn                ; 45h, 46h: removable media only
        cmp     al,02h
        je      lx_chs
        cmp     al,03h
        je      lx_chs
        cmp     al,08h
        je      lx_08
        cmp     al,15h
        je      lx_15
        cmp     al,01h
        je      lx_01
        cmp     al,04h                  ; verify, seek, resets, ready,
        je      lx_ok                   ; recalibrate, diagnostic, park:
        cmp     al,00h                  ; always fine, as INT13.386 has
        je      lx_ok                   ; them
        cmp     al,0Ch
        je      lx_ok
        cmp     al,0Dh
        je      lx_ok
        cmp     al,10h
        je      lx_ok
        cmp     al,11h
        je      lx_ok
        cmp     al,14h
        je      lx_ok
        cmp     al,19h
        je      lx_ok
lx_badfn:
        mov     al,01h
lx_err:
        mov     [ebp+CL_AH],al
        or      byte ptr [ebp+CL_EFLAGS],1
        pop     edi
        clc
        ret
lx_01:
        mov     byte ptr [ebp+CL_AL],0  ; the last operation's status:
lx_ok:                                  ; nothing is remembered
        mov     byte ptr [ebp+CL_AH],0
lx_clc:
        and     byte ptr [ebp+CL_EFLAGS],NOT 1
        pop     edi
        clc
        ret
lx_onpop:
        pop     edi
        jmp     dword ptr [ld_prev13]

; 41h: are they there?  BX=55AAh in, AA55h out; version 1.x, and the
; one subset there is in that version, the fixed-disk calls
lx_41:
        cmp     word ptr [ebp+CL_BX],55AAh
        jne     lx_badfn
        mov     word ptr [ebp+CL_BX],0AA55h
        mov     word ptr [ebp+CL_CX],0001h
        mov     byte ptr [ebp+CL_AH],01h
        jmp     lx_clc

; 42h, 43h: the packet at DS:SI - its size, a count of sectors, the
; buffer's segment:offset and a 64-bit sector number.  A failure gives
; the count back as 0 sectors done.
lx_rw:
        call    lx_dsi
        movzx   ecx,word ptr [esi+2]
        mov     edx,[esi+8]
        cmp     dword ptr [esi+0Ch],0
        jne     lx_range
        or      ecx,ecx
        jz      lx_ok
        mov     eax,edx                 ; the last sector on the drive?
        add     eax,ecx
        jc      lx_range
        dec     eax
        cmp     eax,dword ptr [edi+BD_MAXSEC]
        ja      lx_range
        cmp     dword ptr [esi+4],-1    ; EDD 3's 64-bit flat buffer:
        je      lx_badfn                ; not offered
        push    ebx
        movzx   eax,word ptr [esi+6]
        shl     eax,4
        push    ecx
        movzx   ecx,word ptr [esi+4]
        add     eax,ecx
        pop     ecx
        add     eax,[ebx+CB_HIGHLIN]
        mov     bl,BDC_READ
        cmp     byte ptr [ebp+CL_AH],42h
        je      lx_rwgo
        mov     bl,BDC_WRITE
lx_rwgo:
        push    esi
        call    ld_bdio
        pop     esi
        pop     ebx
        cmp     ax,BDS_FIRST_ERR
        jb      lx_ok
        mov     word ptr [esi+2],0
        mov     cl,04h                  ; sector not found
        cmp     ax,BDS_BAD_SECTOR
        je      lx_rwerr
        mov     cl,0Ah                  ; bad sector, as INT13.386 says
lx_rwerr:
        mov     al,cl
        jmp     lx_err
lx_range:
        mov     word ptr [esi+2],0
        mov     al,04h
        jmp     lx_err

; 02h, 03h: AL sectors from the cylinder, head and sector in CX and DH
; into ES:BX, by the BIOS's geometry, as INT13.386 does it for 80h and
; 81h: the status it gives, and AL = 50h after a read that worked
lx_chs:
        movzx   eax,word ptr [ebp+CL_CX]
        mov     ecx,eax
        shr     al,6
        xchg    al,ah                   ; EAX = the cylinder
        imul    eax,[edi+BD_HEADS]
        movzx   edx,byte ptr [ebp+CL_DH]
        add     eax,edx
        imul    eax,[edi+BD_SPT]
        and     ecx,3Fh
        jz      lx_range2               ; (sectors count from 1)
        lea     edx,[eax+ecx-1]         ; EDX = the first sector
        movzx   ecx,byte ptr [ebp+CL_AL]
        or      ecx,ecx
        jz      lx_ok
        mov     eax,edx
        add     eax,ecx
        jc      lx_range2
        dec     eax
        cmp     eax,dword ptr [edi+BD_MAXSEC]
        ja      lx_range2
        push    ebx
        movzx   eax,word ptr [ebp+CL_ES]
        shl     eax,4
        push    ecx
        movzx   ecx,word ptr [ebp+CL_BX]
        add     eax,ecx
        pop     ecx
        add     eax,[ebx+CB_HIGHLIN]
        mov     bl,BDC_READ
        cmp     byte ptr [ebp+CL_AH],02h
        je      lx_chsgo
        mov     bl,BDC_WRITE
lx_chsgo:
        call    ld_bdio
        pop     ebx
        cmp     ax,BDS_CORRECTED
        je      lx_chsecc
        cmp     ax,BDS_FIRST_ERR
        jae     lx_chserr
        cmp     byte ptr [ebp+CL_AH],02h
        jne     lx_ok
        mov     byte ptr [ebp+CL_AL],50h
        jmp     lx_ok
lx_chsecc:
        mov     al,11h                  ; corrected: the BIOS says so
        jmp     lx_chsfail
lx_chserr:
        mov     cl,04h
        cmp     ax,BDS_BAD_SECTOR
        je      lx_chsstat
        mov     cl,0Ah
lx_chsstat:
        mov     al,cl
lx_chsfail:
        cmp     byte ptr [ebp+CL_AH],02h
        jne     lx_err
        mov     byte ptr [ebp+CL_AL],0
        jmp     lx_err
lx_range2:
        mov     al,04h
        jmp     lx_err

; 08h: the geometry, as the BIOS has it; DL = how many fixed disks the
; BIOS counts (40:75h)
lx_08:
        mov     al,ds:[475h]
        mov     ah,byte ptr [edi+BD_HEADS]
        dec     ah
        mov     [ebp+CL_DX],ax
        mov     ax,word ptr [edi+BD_CYLS]
        sub     ax,2
        shl     ah,6
        or      ah,byte ptr [edi+BD_SPT]
        xchg    ah,al
        mov     [ebp+CL_CX],ax
        jmp     lx_ok

; 15h: a fixed disk, and its sectors in CX:DX
lx_15:
        mov     word ptr [ebp+CL_AX],0300h
        mov     eax,[edi+BD_CYLS]
        dec     eax
        imul    eax,[edi+BD_HEADS]
        imul    eax,[edi+BD_SPT]
        mov     [ebp+CL_DX],ax
        shr     eax,16
        mov     [ebp+CL_CX],ax
        jmp     lx_clc

; 48h: the drive's parameters into the buffer at DS:SI, whose first
; word is its size: 1Ah bytes, or 1Eh with no device table
lx_48:
        call    lx_dsi
        movzx   eax,word ptr [esi]
        cmp     eax,1Ah
        jb      lx_badfn
        mov     ecx,1Ah
        cmp     eax,1Eh
        jb      lx_48size
        mov     dword ptr [esi+1Ah],-1
        mov     ecx,1Eh
lx_48size:
        mov     word ptr [esi],cx
        mov     word ptr [esi+2],2      ; the geometry is filled in
        mov     eax,[edi+BD_HEADS]
        mov     [esi+8],eax
        mov     ecx,[edi+BD_SPT]
        mov     [esi+0Ch],ecx
        imul    ecx,eax                 ; sectors a cylinder
        mov     eax,dword ptr [edi+BD_MAXSEC]
        inc     eax
        mov     [esi+10h],eax
        mov     dword ptr [esi+14h],0
        xor     edx,edx
        div     ecx
        mov     [esi+4],eax
        mov     word ptr [esi+18h],512
        jmp     lx_ok

lx_dsi:                                 ; ESI -> the VM's DS:SI
        movzx   esi,word ptr [ebp+CL_DS]
        shl     esi,4
        push    eax
        movzx   eax,word ptr [ebp+CL_SI]
        add     esi,eax
        pop     eax
        add     esi,[ebx+CB_HIGHLIN]
        ret

;---------------------------------------------------------------------
; ld_bdio - a transfer for a VM through BlockDev, waited for: EDI ->
; the drive's descriptor, EDX = the first sector, ECX = sectors, EAX =
; the buffer's linear address, BL = BDC_READ or BDC_WRITE.  AX =
; BlockDev's status.  The buffer's pages are locked for it, as
; INT13.386 locks them; pages that will not lock go one sector at a
; time through ld_bounce.  The command block is on the stack, with a
; semaphore of its own that the completion signals.  ECX, EDX and ESI
; changed.
;---------------------------------------------------------------------
ld_bdio:
        push    ebp
        sub     esp,CB_SIZE
        mov     ebp,esp
        mov     dword ptr [ebp+CB_NEXT],0
        movzx   esi,bl
        mov     word ptr [ebp+CB_COMMAND],si
        mov     word ptr [ebp+CB_STATUS],0
        mov     dword ptr [ebp+CB_FLAGS],BDCF_HIGH
        mov     dword ptr [ebp+CB_CPLT],OFFSET FLAT:ld_bdcplt
        mov     dword ptr [ebp+CB_SECTOR],edx
        mov     dword ptr [ebp+CB_SECTOR+4],0
        mov     dword ptr [ebp+CB_COUNT],ecx
        mov     dword ptr [ebp+CB_BUFFER],eax
        push    eax
        push    ecx
        xor     ecx,ecx                 ; no tokens yet
        VXDCALL S_CREATE_SEM
        mov     [ebp+CB_RESCLIENT],eax
        pop     ecx
        pop     eax
        jc      lbd_nosem
        push    eax
        VXDCALL S_ENABLE_VM_INTS        ; so the lock can page in
        pop     eax

        mov     esi,eax                 ; the pages it covers
        and     esi,0FFFh
        mov     edx,ecx
        shl     edx,9
        lea     edx,[esi+edx+0FFFh]
        shr     edx,12
        mov     esi,eax
        shr     esi,12
        push    edx                     ; (kept for the unlock)
        push    esi
        push    0
        push    edx
        push    esi
        VXDCALL S_LINPAGELOCK
        add     esp,12
        or      eax,eax
        jz      lbd_bounce
        call    lbd_send
        pop     esi
        pop     edx
        push    0
        push    edx
        push    esi
        VXDCALL S_LINPAGEUNLOCK
        add     esp,12
        jmp     lbd_done

lbd_bounce:
        pop     esi
        pop     edx
        mov     ecx,[ebp+CB_COUNT]
        mov     esi,[ebp+CB_BUFFER]
        mov     dword ptr [ebp+CB_COUNT],1
        mov     dword ptr [ebp+CB_BUFFER],OFFSET FLAT:ld_bounce
lbd_one:
        push    ecx
        push    esi
        cmp     word ptr [ebp+CB_COMMAND],BDC_READ
        je      lbd_send1
        push    edi
        mov     edi,OFFSET FLAT:ld_bounce
        mov     ecx,128
        cld
        rep     movsd
        pop     edi
lbd_send1:
        call    lbd_send
        pop     esi
        pop     ecx
        cmp     word ptr [ebp+CB_STATUS],BDS_FIRST_ERR
        jae     lbd_done
        cmp     word ptr [ebp+CB_COMMAND],BDC_READ
        jne     lbd_next
        push    ecx
        push    esi
        push    edi
        mov     edi,esi
        mov     esi,OFFSET FLAT:ld_bounce
        mov     ecx,128
        cld
        rep     movsd
        pop     edi
        pop     esi
        pop     ecx
lbd_next:
        add     esi,512
        inc     dword ptr [ebp+CB_SECTOR]
        loop    lbd_one

lbd_done:
        mov     eax,[ebp+CB_RESCLIENT]
        VXDCALL S_DESTROY_SEM
        movzx   eax,word ptr [ebp+CB_STATUS]
lbd_out:
        add     esp,CB_SIZE
        pop     ebp
        ret
lbd_nosem:
        mov     eax,BDS_DEVICE_ERR
        jmp     lbd_out

lbd_send:                               ; EBP -> the block: send, wait
        push    esi
        mov     dword ptr [ebp+CB_NEXT],0
        mov     esi,ebp
        VXDCALL S_BD_SEND_COMMAND
        mov     eax,[ebp+CB_RESCLIENT]
        mov     ecx,BLOCK_SVC_INTS OR BLOCK_ENABLE_INTS
        VXDCALL S_WAIT_SEM
        pop     esi
        ret

ld_bdcplt:                              ; BD_CB_Cmd_Cplt_Proc: ESI -> it
        mov     eax,[esi+CB_RESCLIENT]
        VXDCALL S_SIGNAL_SEM
        ret

;---------------------------------------------------------------------
; ld_tick - a global time-out every LD_LOSTMS.  A controller that has
; had a command that long is looked at (ld_kick): if the drive is no
; longer busy the command's interrupt was lost, and the command goes
; on from here.  One it has had for LD_STUCKMS is said so, and again
; that long after.  EBX = the current VM.
;---------------------------------------------------------------------
ld_tick:
        push    ebp
        mov     ebp,OFFSET FLAT:ld_cbl0
        call    ltk_cable
        mov     ebp,OFFSET FLAT:ld_cbl1
        call    ltk_cable
        pop     ebp
IFDEF LRG_TRACE
        call    ld_tstats
ENDIF
        mov     eax,LD_LOSTMS
        mov     esi,OFFSET FLAT:ld_tick
        VXDCALL S_SET_GLOBAL_TIME_OUT
        ret
ltk_cable:
        mov     ecx,[ebp+CBL_CMDTIME]
        or      ecx,ecx
        jz      ltk_out
        VXDCALL S_GET_TIME
        sub     eax,ecx
        jb      ltk_out
        cmp     eax,LD_LOSTMS
        jbe     ltk_out
        pushfd
        cli
        call    ld_kick
        popfd
        cmp     [ebp+CBL_CMDTIME],ecx   ; (that ended it, or moved it on)
        jne     ltk_out
        cmp     eax,LD_STUCKMS
        jbe     ltk_out
        TRC     58h             ; X
        VXDCALL S_GET_TIME              ; (counted from now, for the next
        mov     [ebp+CBL_CMDTIME],eax   ; time it is said)
        push    ebp
        mov     ebp,[esp+8]             ; the VM's registers, for the
        mov     edi,OFFSET FLAT:ld_title ; shell
        mov     eax,LD_MBFLAGS
        mov     ecx,OFFSET FLAT:ld_stuck
        VXDCALL S_SHELL_SYSMODAL_MSG
        pop     ebp
ltk_out:
        ret

;---------------------------------------------------------------------
; ld_iotrap - a VM touched a cable's ports, or the engine's.  EBX = the
; VM, ECX = the kind of I/O, EDX = the port, EAX = what it wrote; EAX =
; what it reads.  Strings, and words and dwords anywhere but the data
; port, go back to the VMM to be split up (Simulate_IO calls here
; again for each piece).
;
; With nothing on the cable but LRGDISK's drives (CF_OTHER clear), and
; for the engine, none of it reaches the controller: each byte reads
; FFh and a write goes nowhere, with a message the first time.  With
; something else there, ld_vmio shares the cable.  EBX, EBP kept.
;---------------------------------------------------------------------
ld_iotrap:
        test    ecx,STRING_IO OR REP_IO
        jnz     lit_simulate
        push    ebp
        push    esi
        mov     ebp,OFFSET FLAT:ld_cbl0 ; which cable, and which register
        call    ld_regidx
        jnc     lit_cable
        mov     ebp,OFFSET FLAT:ld_cbl1
        call    ld_regidx
        jnc     lit_cable
        jmp     lit_shut                ; the engine's
lit_cable:
        cmp     byte ptr [ld_rawbios],0 ; LRGDISK's own question to the
        jne     lit_simpop              ; BIOS (ld_biosgeo): whatever the
                                        ; BIOS does at the ports, it does
        test    byte ptr [ebp+CBL_FLAGS],CF_OTHER
        jz      lit_shut
        cmp     ecx,BYTE_OUTPUT
        jbe     lit_shared
        or      esi,esi                 ; (words only at the data port)
        jz      lit_shared
        pop     esi
        pop     ebp
lit_simulate:
        VXDCALL S_SIMULATE_IO
        ret
lit_shared:
        pushfd
        cli
        call    ld_vmio
        popfd
        cmp     byte ptr [ld_tell],0
        je      lit_out
        mov     byte ptr [ld_tell],0
        call    ld_refused
        jmp     lit_out
lit_shut:
        cmp     ecx,BYTE_OUTPUT
        ja      lit_simpop
        call    ld_refused
        mov     al,0FFh
lit_out:
        pop     esi
        pop     ebp
        ret
lit_simpop:
        pop     esi
        pop     ebp
        jmp     lit_simulate

; ld_regidx - ESI = which register of cable EBP port EDX is: 0 to 7
; from the data port, R_CTRL for the control port; CF if the port is
; not the cable's
ld_regidx:
        movzx   esi,word ptr [ebp+CBL_DATA]
        sub     esi,edx
        neg     esi
        cmp     esi,7
        jbe     lrx_ok
        mov     esi,R_CTRL
        cmp     dx,[ebp+CBL_CTRL]
        je      lrx_ok
        stc
        ret
lrx_ok:
        clc
        ret

; ld_refused - the message, once: a program tried to drive one of
; LRGDISK's disks itself.  Called from ld_iotrap, with the VM's
; registers pointer (which the shell's message needs in EBP: the VMM
; runs the VM for it) two slots up the stack.  Nothing changed.
ld_refused:
        TRC     50h             ; P
        TRCHEX  edx, 3
        TRCHEX  ecx, 1
        push    eax
        push    ecx
        push    edi
        push    ebp
        mov     al,1
        xchg    al,[ld_warned]
        or      al,al
        jnz     lrf_out
        mov     ebp,[esp+24]            ; the VM's registers
        xor     edi,edi
        mov     eax,LD_MBFLAGS
        mov     ecx,OFFSET FLAT:ld_portmsg
        VXDCALL S_SHELL_SYSMODAL_MSG
lrf_out:
        pop     ebp
        pop     edi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_vmio - one byte, or a word or dword of the data port, for a VM on
; a shared cable; interrupts off.  As ld_iotrap, with EBP -> the cable
; and ESI = the register.
;
; The cable is the VMs' while LRGDISK has no command on it, and while
; one of theirs is on it (CBL_VMBUSY): their I/O goes to the
; controller, once what they last left in it is put back (ld_restore)
; if LRGDISK has used it since.  A command they write makes it theirs
; until a read of the status shows it over (neither BSY nor DRQ) - or
; until the watch finds it over.  While LRGDISK has it, what they write
; is kept, a command waits (CBL_VMWAIT, sent by ld_lend when LRGDISK's
; is done; a reset too, done for them then) and a read finds what they
; last saw, the status BSY while their command waits.
;
; Whatever the cable, LRGDISK's own units stay out of their reach: with
; one of them selected, a read finds FFh, a write is kept and goes no
; further, and a command is refused with the message.
;---------------------------------------------------------------------
ld_vmio:
        cmp     dword ptr [ebp+CBL_VMBUSY],0
        jne     lvi_theirs
        cmp     dword ptr [ebp+CBL_CUR],0
        jne     lvi_ld
        cmp     byte ptr [ebp+CBL_VMWAIT],0 ; (theirs waits for the other
        jne     lvi_ld                  ; cable: as if LRGDISK had this)
lvi_theirs:
        test    ecx,IO_OUTPUT
        jnz     lvi_write
        call    ld_unitours
        jc      lvi_ff
        cmp     byte ptr [ebp+CBL_HWVM],0
        jne     lvi_read
        call    ld_restore
lvi_read:
        cmp     ecx,WORD_INPUT
        je      lvi_rdw
        ja      lvi_rdd
        in      al,dx
        jmp     ld_vmsaw
lvi_rdw:
        in      ax,dx
        ret
lvi_rdd:
        in      eax,dx
        ret
lvi_ff:
        or      eax,-1
        ret

lvi_write:
        call    ld_vmwrote
        call    ld_unitours
        jc      lvi_wours
        cmp     byte ptr [ebp+CBL_HWVM],0
        jne     lvi_wsend
        call    ld_restore              ; (what was just written included -
        cmp     esi,R_CMD               ; but not a reset, which ld_restore
        je      lvi_wsend               ; never gives: the control port's
        cmp     esi,R_CTRL              ; byte goes out itself, and a reset
        je      lvi_wsend               ; in it is marked)
        or      esi,esi
        jz      lvi_wsend
        ret
lvi_wsend:
        cmp     esi,R_CTRL
        jne     lvi_wport
        test    al,CTL_SRST             ; a reset reaches LRGDISK's drives
        jz      lvi_wport               ; too: they are set up again
        mov     byte ptr [ebp+CBL_RESET],1 ; before its next command
        TRC     2Ah             ; *
lvi_wport:
        cmp     ecx,WORD_INPUT OR IO_OUTPUT
        je      lvi_ww
        ja      lvi_wd
        cmp     esi,R_CMD
        jne     lvi_wbyte
        cmp     byte ptr [ld_serial],0  ; one cable at a time: theirs
        je      lvi_wcmd                ; waits while the other has one
        call    ld_otherbusy
        jc      lvi_ldcmd
lvi_wcmd:
        out     dx,al
        mov     [ebp+CBL_VMBUSY],ebx    ; a command: theirs until it is
        push    eax                     ; over
        VXDCALL S_GET_TIME
        mov     [ebp+CBL_VMWHEN],eax
        pop     eax
        TRC     76h             ; v
lvi_wout:
        ret
lvi_wbyte:
        out     dx,al
        ret
lvi_ww:
        out     dx,ax
        ret
lvi_wd:
        out     dx,eax
        ret
lvi_wours:
        mov     byte ptr [ebp+CBL_HWVM],0 ; (kept, so the controller
        cmp     esi,R_CMD               ; differs)
        jne     lvi_wout
        mov     byte ptr [ld_tell],1
        ret

        ; LRGDISK has the cable
lvi_ld:
        test    ecx,IO_OUTPUT
        jnz     lvi_ldw
        call    ld_unitours
        jc      lvi_ff
        or      esi,esi
        jz      lvi_ff                  ; the data port: nothing there
        cmp     esi,R_STATUS
        jae     lvi_ldst
        mov     al,[ebp+esi+CBL_VR-1]
        ret
lvi_ldst:
        mov     al,[ebp+CBL_VST]
        cmp     byte ptr [ebp+CBL_VMWAIT],0
        je      lvi_ldout
        mov     al,ST_BSY
lvi_ldout:
        ret
lvi_ldw:
        call    ld_vmwrote
        cmp     esi,R_CTRL
        jne     lvi_ldcmd
        test    al,CTL_SRST             ; a reset, to be done for them
        jz      lvi_ldout               ; when LRGDISK's command is over
        mov     byte ptr [ebp+CBL_VMSRST],1
        mov     [ebp+CBL_VMWAITVM],ebx
        mov     byte ptr [ebp+CBL_VMWAIT],1
        TRC     2Ah             ; *
        ret
lvi_ldcmd:
        cmp     esi,R_CMD
        jne     lvi_ldout
        call    ld_unitours
        jc      lvi_wours
        mov     [ebp+CBL_VMCMD],al      ; their command, after LRGDISK's
        mov     [ebp+CBL_VMWAITVM],ebx
        mov     byte ptr [ebp+CBL_VMWAIT],1
        TRC     71h             ; q
        ret

; ld_unitours - CF if the unit the VMs have selected is LRGDISK's
ld_unitours:
        push    eax
        movzx   eax,byte ptr [ebp+CBL_VW+5]
        shr     eax,4
        and     eax,1
        bt      dword ptr [ebp+CBL_OURS],eax
        pop     eax
        ret

; ld_vmwrote - a byte a VM wrote, AL to register ESI, into CBL_VW and
; CBL_VWHOB (two deep, as the controller keeps +1 to +5 for a 48-bit
; command) and into CBL_VR, as a read would find it; the control port
; into CBL_VCTL.  Nothing changed.
ld_vmwrote:
        cmp     ecx,IO_OUTPUT           ; bytes only
        jne     lvw_out
        cmp     esi,R_CTRL
        je      lvw_ctl
        or      esi,esi
        jz      lvw_out
        cmp     esi,R_DRVHD
        ja      lvw_out                 ; (a command is not kept here)
        push    ecx
        cmp     esi,R_DRVHD
        je      lvw_now
        mov     cl,[ebp+esi+CBL_VW-1]
        mov     [ebp+esi+CBL_VWHOB-1],cl
lvw_now:
        mov     [ebp+esi+CBL_VW-1],al
        cmp     esi,R_FEAT              ; (+1 reads back the error)
        je      lvw_done
        mov     [ebp+esi+CBL_VR-1],al
lvw_done:
        pop     ecx
lvw_out:
        ret
lvw_ctl:
        mov     [ebp+CBL_VCTL],al
        ret

; ld_vmsaw - a byte a VM read, AL from register ESI, remembered; a
; status read that shows their command over gives the cable back
ld_vmsaw:
        cmp     esi,R_STATUS
        je      lvs_status
        cmp     esi,R_CTRL
        je      lvs_alt
        or      esi,esi
        jz      lvs_out
        mov     [ebp+esi+CBL_VR-1],al
lvs_out:
        ret
lvs_alt:
        mov     [ebp+CBL_VST],al
        ret
lvs_status:
        mov     [ebp+CBL_VST],al
        cmp     dword ptr [ebp+CBL_VMBUSY],0
        je      lvs_out
        test    al,ST_BSY OR ST_DRQ
        jnz     lvs_out
        push    eax
        call    ld_release
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_restore - what the VMs last left in the controller, back in it:
; their unit selected first, the device control (never a reset), then
; +1 to +5 twice each, the earlier write first.  Interrupts off.
; Nothing changed.
;---------------------------------------------------------------------
ld_restore:
        push    eax
        push    ecx
        push    edx
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_DRVHD
        mov     al,[ebp+CBL_VW+5]
        out     dx,al
        IODELAY
        IODELAY
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,[ebp+CBL_VCTL]
        and     al,NOT CTL_SRST
        out     dx,al
        movzx   edx,word ptr [ebp+CBL_DATA]
        inc     edx
        mov     al,[ebp+CBL_VWHOB]
        out     dx,al
        mov     al,[ebp+CBL_VW]
        out     dx,al
        xor     ecx,ecx
lrs_reg:
        inc     ecx
        inc     edx                     ; +2 to +5
        mov     al,[ebp+ecx+CBL_VWHOB]
        out     dx,al
        mov     al,[ebp+ecx+CBL_VR]
        out     dx,al
        cmp     ecx,4
        jb      lrs_reg
        mov     byte ptr [ebp+CBL_HWVM],1
        TRC     2Bh             ; +
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_snapshot - LRGDISK is about to use the controller, which holds
; what the VMs left: +1 to +6 and the status kept as a read of them
; would find them, for the VMs to read while LRGDISK has the cable and
; for ld_restore.  Interrupts off.  Nothing changed.
;---------------------------------------------------------------------
ld_snapshot:
        push    eax
        push    ecx
        push    edx
        movzx   edx,word ptr [ebp+CBL_DATA]
        xor     ecx,ecx
lsn_reg:
        inc     ecx
        inc     edx
        in      al,dx
        mov     [ebp+ecx+CBL_VR-1],al
        cmp     ecx,R_DRVHD
        jb      lsn_reg
        movzx   edx,word ptr [ebp+CBL_CTRL]
        in      al,dx
        mov     [ebp+CBL_VST],al
        mov     byte ptr [ebp+CBL_HWVM],0
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_lend - the VMs' waiting command onto the cable, LRGDISK having
; none on it; or their reset, done for them: SRST held long enough and
; let go, and LRGDISK's drives to be set up again (CBL_RESET).  Either
; way the cable is theirs until their status read shows the drive
; ready.  Interrupts off.  Nothing changed.
;---------------------------------------------------------------------
ld_lend:
        cmp     byte ptr [ld_serial],0  ; one cable at a time: not while
        je      lld_go                  ; the other has a command (the
        call    ld_otherbusy            ; other's ld_xrelease lends it)
        jnc     lld_go
        ret
lld_go:
        push    eax
        push    ecx
        push    edx
        mov     byte ptr [ebp+CBL_VMWAIT],0
        call    ld_restore
        cmp     byte ptr [ebp+CBL_VMSRST],0
        jne     lld_srst
        mov     al,[ebp+CBL_VMCMD]
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_CMD
        out     dx,al
lld_theirs:
        mov     eax,[ebp+CBL_VMWAITVM]
        mov     [ebp+CBL_VMBUSY],eax
        VXDCALL S_GET_TIME
        mov     [ebp+CBL_VMWHEN],eax
        TRC     76h             ; v
        pop     edx
        pop     ecx
        pop     eax
        ret
lld_srst:
        mov     byte ptr [ebp+CBL_VMSRST],0
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,[ebp+CBL_VCTL]
        or      al,CTL_SRST
        out     dx,al
        mov     ecx,16                  ; 5us and more
lld_hold:
        IODELAY
        in      al,dx
        loop    lld_hold
        mov     al,[ebp+CBL_VCTL]
        and     al,NOT CTL_SRST
        out     dx,al
        mov     byte ptr [ebp+CBL_RESET],1
        TRC     2Ah             ; *
        jmp     lld_theirs

;---------------------------------------------------------------------
; ld_vmcheck - is the VMs' command over?  The alternate status of their
; unit (still selected), which leaves its interrupt alone: neither BSY
; nor DRQ, and the cable is no longer theirs.  CF if it is still on.
; Interrupts off.  Nothing changed.
;---------------------------------------------------------------------
ld_vmcheck:
        push    eax
        push    edx
        movzx   edx,word ptr [ebp+CBL_CTRL]
        in      al,dx
        test    al,ST_BSY OR ST_DRQ
        stc
        jnz     lvc_out
        mov     [ebp+CBL_VST],al
        mov     dword ptr [ebp+CBL_VMBUSY],0
        clc
lvc_out:
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_release - the VMs' command is over: the cable is LRGDISK's again,
; and the command it held goes out.  Interrupts off.  Nothing changed.
;---------------------------------------------------------------------
ld_release:
        mov     dword ptr [ebp+CBL_VMBUSY],0
        cmp     byte ptr [ebp+CBL_HELD],0
        je      lrl_other
        mov     byte ptr [ebp+CBL_HELD],0
        pushad
        mov     esi,[ebp+CBL_CUR]
        mov     edi,[ebp+CBL_CURBDD]
        call    ld_go
        popad
lrl_other:
        pushad
        call    ld_xrelease
        popad
        ret

;---------------------------------------------------------------------
; ld_go - the command in hand goes out (ld_begin): ESI -> it, EDI ->
; its descriptor, EBP -> the cable - unless the cables are used one at
; a time and the other has a command on it, in which case it waits
; (CBL_XHELD) for the other cable's ld_xrelease.  Interrupts off.  EAX,
; ECX, EDX changed.
;---------------------------------------------------------------------
ld_go:
        cmp     byte ptr [ld_serial],0
        je      ld_begin
        call    ld_otherbusy
        jnc     ld_begin
        mov     byte ptr [ebp+CBL_XHELD],1
        TRC     78h             ; x
        ret

; ld_otherbusy - CF if the other cable has a command on it, LRGDISK's
; (CBL_CMDTIME) or a DOS program's (CBL_VMBUSY).  Nothing changed.
ld_otherbusy:
        push    eax
        mov     eax,OFFSET FLAT:ld_cbl0
        add     eax,OFFSET FLAT:ld_cbl1
        sub     eax,ebp
        cmp     dword ptr [eax+CBL_CMDTIME],0
        jne     lob_busy
        cmp     dword ptr [eax+CBL_VMBUSY],0
        jne     lob_busy
        pop     eax
        clc
        ret
lob_busy:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; ld_xrelease - this cable (EBP) has gone idle: with the cables used
; one at a time, the other cable's waiting command goes out - LRGDISK's
; held one (CBL_XHELD), else a DOS program's (CBL_VMWAIT, lent).
; Interrupts off.  EAX, ECX, EDX changed.
;---------------------------------------------------------------------
ld_xrelease:
        cmp     byte ptr [ld_serial],0
        je      lxr_out
        cmp     dword ptr [ebp+CBL_CMDTIME],0
        jne     lxr_out
        cmp     dword ptr [ebp+CBL_VMBUSY],0
        jne     lxr_out
        push    ebp
        push    esi
        push    edi
        mov     eax,OFFSET FLAT:ld_cbl0
        add     eax,OFFSET FLAT:ld_cbl1
        sub     eax,ebp
        mov     ebp,eax                 ; EBP -> the other cable
        cmp     byte ptr [ebp+CBL_XHELD],0
        je      lxr_vm
        mov     byte ptr [ebp+CBL_XHELD],0
        mov     esi,[ebp+CBL_CUR]
        mov     edi,[ebp+CBL_CURBDD]
        call    ld_begin
        jmp     lxr_done
lxr_vm:
        cmp     byte ptr [ebp+CBL_VMWAIT],0
        je      lxr_done
        cmp     dword ptr [ebp+CBL_CUR],0
        jne     lxr_done
        call    ld_lend
lxr_done:
        pop     edi
        pop     esi
        pop     ebp
lxr_out:
        ret

;---------------------------------------------------------------------
; ld_watchon - while LRGDISK holds a command, a look at the cable every
; LD_WATCHMS (ld_watchtick, with the cable as its reference data): a
; VM's command can end without its status being read - a program that
; polls the alternate status, or a DOS box closed in the middle.
; After LD_LENDMS the cable is taken back anyway.  Interrupts off.
; Nothing changed.
;---------------------------------------------------------------------
ld_watchon:
        cmp     dword ptr [ebp+CBL_WATCH],0
        jne     lwo_out
        push    eax
        push    edx
        push    esi
        mov     eax,LD_WATCHMS
        mov     edx,ebp
        mov     esi,OFFSET FLAT:ld_watchtick
        VXDCALL S_SET_GLOBAL_TIME_OUT
        mov     [ebp+CBL_WATCH],esi
        pop     esi
        pop     edx
        pop     eax
lwo_out:
        ret

ld_watchtick:
        pushfd
        cli
        push    ebp
        mov     ebp,edx
        mov     dword ptr [ebp+CBL_WATCH],0
        cmp     byte ptr [ebp+CBL_HELD],0
        je      lwt_out
        call    ld_vmcheck
        jnc     lwt_take
        VXDCALL S_GET_TIME
        sub     eax,[ebp+CBL_VMWHEN]
        cmp     eax,LD_LENDMS
        jb      lwt_again
        TRC     56h             ; V
lwt_take:
        call    ld_release
        jmp     lwt_out
lwt_again:
        call    ld_watchon
lwt_out:
        pop     ebp
        popfd
        ret

IFDEF LRG_TRACE
;---------------------------------------------------------------------
; ld_tchar - AL to LPT1, waiting a moment for the printer not to be
; busy, and to COM1 (set up by ld_syscrit: 115200, 8N1) when its
; holding register is empty; ld_thex - EAX as CL hex digits.  Nothing
; changed.
;---------------------------------------------------------------------
ld_tchar:
        push    ecx
        push    edx
        push    eax
        mov     edx,379h
        mov     ecx,100000
ltc_wait:
        in      al,dx
        test    al,80h                  ; not busy
        jnz     ltc_go
        loop    ltc_wait
ltc_go:
        pop     eax
        push    eax
        mov     edx,378h
        out     dx,al
        mov     edx,37Ah
        mov     al,0Dh                  ; strobe
        out     dx,al
        IODELAY
        mov     al,0Ch
        out     dx,al
        mov     edx,3FDh
        mov     ecx,100000
ltc_wait2:
        in      al,dx
        test    al,20h                  ; holding register empty
        jnz     ltc_go2
        loop    ltc_wait2
ltc_go2:
        pop     eax
        push    eax
        mov     edx,3F8h
        out     dx,al
        pop     eax
        pop     edx
        pop     ecx
        ret

ld_thex:
        push    eax
        push    ecx
        push    edx
        mov     edx,eax
        movzx   ecx,cl
lth_digit:
        push    ecx
        dec     ecx
        shl     ecx,2
        mov     eax,edx
        shr     eax,cl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     lth_put
        add     al,'A'-'0'-10
lth_put:
        call    ld_tchar
        pop     ecx
        loop    lth_digit
        pop     edx
        pop     ecx
        pop     eax
        ret

; ld_tstats - the counts (ld_st, in SC_* order) as a line that starts
; with '#', if any of them has moved since the last one.  Nothing
; changed.
ld_tstats:
        push    eax
        push    ecx
        push    esi
        xor     eax,eax
        xor     ecx,ecx
lts_sum:
        add     eax,[ld_st+ecx*4]
        inc     ecx
        cmp     ecx,SC_COUNT
        jb      lts_sum
        cmp     eax,[ld_stsaid]
        je      lts_out
        mov     [ld_stsaid],eax
        TRC     13
        TRC     10
        TRC     23h             ; #
        xor     esi,esi
lts_one:
        TRC     20h
        TRCHEX  [ld_st+esi*4], 8
        inc     esi
        cmp     esi,SC_COUNT
        jb      lts_one
        TRC     13
        TRC     10
lts_out:
        pop     esi
        pop     ecx
        pop     eax
        ret
ENDIF
_LTEXT  ENDS

;=====================================================================
; START-UP, PROTECTED MODE (discarded afterwards)
;
; Sys_Critical_Init does nearly all of it, as WDCTRL does - BlockDev
; decides at its own Device_Init, from the drives registered by then,
; whether to hook the VMs' INT 13h at all.  What it cannot do is ask
; the BIOS the geometry: that needs a VM to run in, which Device_Init
; has; so the drives are registered with a stand-in geometry and told
; the BIOS's at Device_Init, before anything reads a sector (LRGDISK's
; init order puts its Device_Init right after VPICD's, ahead of
; PageFile's, which is the first to read the disk).  No memory can be
; handed from the real-mode start-up to here - WIN386's loader owns all
; of DOS memory and all of extended memory by the time a real-mode
; start-up runs, and reuses the start-up's own segment for the next
; device's - so the start-up says in EDX what it decided, a byte a unit,
; and what it measured is asked for again here.
;=====================================================================
_IDATA  SEGMENT
ld_ref          dd      0               ; the reference data
ld_geo          dw      0               ; AH=08h: heads, sectors, the
                db      0               ; highest cylinder
                dw      0
ld_ident        dw      256 dup (0)     ; IDENTIFY DEVICE's answer
ld_id32         dd      128 dup (0)     ; ...taken a doubleword at a time
ld_blockon      db      0               ; LRGDiskBlockMode is not off
ld_io32on       db      0               ; LRGDisk32BitIO is not off, and
                                        ; the controller is no CMD640
ld_pciide       db      0               ; the controller is a PCI one
ld_kblock       db      'LRGDISKBLOCKMODE', 0
ld_kpoll        db      'LRGDISKPOLL', 0
ld_kio32        db      'LRGDISK32BITIO', 0
ld_initerr      db      'LRGDISK could not register a hard disk with '
                db      'Windows.', 0
_IDATA  ENDS

_ITEXT  SEGMENT
        ASSUME  ds:FLAT, es:FLAT, ss:FLAT

;---------------------------------------------------------------------
; ld_syscrit - Sys_Critical_Init: EDX = the reference data.  This
; fills in a descriptor for each drive the real-mode start-up took,
; gives them to BlockDev, takes the cables' ports from the VMs, the
; second cable's interrupt from VPICD and a page for the engine's
; tables from the VMM.
;---------------------------------------------------------------------
ld_syscrit:
        mov     [ld_ref],edx
IFDEF LRG_TRACE
        push    edx                     ; COM1: 115200, 8N1, DTR and RTS
        mov     dx,3FBh
        mov     al,80h
        out     dx,al
        mov     dx,3F8h
        mov     al,1
        out     dx,al
        inc     dx
        xor     al,al
        out     dx,al
        mov     dx,3FBh
        mov     al,3
        out     dx,al
        mov     dx,3FCh
        out     dx,al
        pop     edx
ENDIF
        TRC     13
        TRC     10
        TRC     4Ch             ; L
        TRCHEX  edx, 8
        TRC     13
        TRC     10
        xor     al,al                   ; the whole-driver bits
        test    dl,RF_SERIAL
        jz      ldi_serial
        inc     al
ldi_serial:
        mov     [ld_serial],al
        xor     al,al
        test    dh,RF_DMAON
        jz      ldi_dma
        inc     al
ldi_dma:
        mov     [ld_dmaon],al

        ; How PIO goes: SYSTEM.INI's three switches, on unless they
        ; say off, and what the real-mode start-up found of the
        ; controller - a PCI one, where a doubleword read of the data
        ; port is worth trying, and a CMD640, where it never is
        mov     eax,1
        xor     esi,esi                 ; [386Enh]
        mov     edi,OFFSET FLAT:ld_kblock
        VXDCALL S_GET_PROFILE_BOOLEAN
        mov     [ld_blockon],al
        mov     eax,1
        xor     esi,esi
        mov     edi,OFFSET FLAT:ld_kpoll
        VXDCALL S_GET_PROFILE_BOOLEAN
        mov     [ld_pollon],al
        mov     eax,1
        xor     esi,esi
        mov     edi,OFFSET FLAT:ld_kio32
        VXDCALL S_GET_PROFILE_BOOLEAN
        test    dword ptr [ld_ref],RF_NO32 SHL 24
        jz      ldi_io32
        xor     al,al
ldi_io32:
        mov     [ld_io32on],al
        xor     al,al
        test    dword ptr [ld_ref],RF_PCIIDE SHL 16
        jz      ldi_pci
        inc     al
ldi_pci:
        mov     [ld_pciide],al

        ; the cables' fixed parts
        mov     ebp,OFFSET FLAT:ld_cbl0
        mov     word ptr [ebp+CBL_DATA],IDE0_DATA
        mov     word ptr [ebp+CBL_CTRL],IDE0_CTRL
        mov     byte ptr [ebp+CBL_NUM],0
        call    lsi_cblinit
        mov     ebp,OFFSET FLAT:ld_cbl1
        mov     word ptr [ebp+CBL_DATA],IDE1_DATA
        mov     word ptr [ebp+CBL_CTRL],IDE1_CTRL
        mov     byte ptr [ebp+CBL_NUM],1
        call    lsi_cblinit

        ; the engine, and its page: two tables, one a cable, physically
        ; where the page is (a page never crosses a 64K boundary)
        cmp     byte ptr [ld_dmaon],0
        je      lsi_noengine
        call    ld_findbm
        jc      lsi_noengine
        push    PAGEFIXED OR PAGEZEROINIT
        push    0                       ; PhysAddr
        push    0                       ; maxPhys
        push    0                       ; minPhys
        push    0                       ; AlignMask
        push    0                       ; VM
        push    PG_SYS
        push    1                       ; nPages
        VXDCALL S_PAGEALLOCATE
        add     esp,32
        or      eax,eax
        jz      lsi_noengine
        mov     esi,edx                 ; ESI = the page, linear
        shr     edx,12
        push    0
        push    OFFSET FLAT:ld_ptes
        push    1
        push    edx
        VXDCALL S_COPYPAGETABLE
        add     esp,16
        or      eax,eax
        jz      lsi_noengine
        mov     eax,[ld_ptes]
        and     eax,0FFFFF000h          ; EAX = the page, physical
        mov     dword ptr [ld_cbl0+CBL_PRD],esi
        mov     dword ptr [ld_cbl0+CBL_PRDPHYS],eax
        add     esi,2048
        add     eax,2048
        mov     dword ptr [ld_cbl1+CBL_PRD],esi
        mov     dword ptr [ld_cbl1+CBL_PRDPHYS],eax
        jmp     lsi_engine
lsi_noengine:
        mov     byte ptr [ld_dmaon],0
        mov     word ptr [ld_bmbase],0
        mov     word ptr [ld_cbl0+CBL_BM],0
        mov     word ptr [ld_cbl1+CBL_BM],0
lsi_engine:

        ; each cable with a drive of LRGDISK's on it
        mov     ebp,OFFSET FLAT:ld_cbl0
        mov     esi,[ld_ref]            ; SI = its two units' bytes
        mov     edi,OFFSET FLAT:ld_bdd
        mov     edx,OFFSET FLAT:ld_names
        call    lsi_cable
        mov     ebp,OFFSET FLAT:ld_cbl1
        mov     esi,[ld_ref]
        shr     esi,16
        mov     edi,OFFSET FLAT:ld_bdd+2*BP_SIZE
        mov     edx,OFFSET FLAT:ld_names+18
        call    lsi_cable

        ; the engine's ports, if it is in use: nobody else's
        cmp     word ptr [ld_bmbase],0
        je      lsi_nobmtrap
        movzx   edx,word ptr [ld_bmbase]
        mov     ecx,16
        mov     esi,OFFSET FLAT:ld_iotrap
lsi_bmport:
        VXDCALL S_INSTALL_IO_HANDLER
        inc     edx
        loop    lsi_bmport
lsi_nobmtrap:

        mov     eax,LD_LOSTMS
        mov     esi,OFFSET FLAT:ld_tick
        VXDCALL S_SET_GLOBAL_TIME_OUT

        ; INT 13h's extensions for the drives, and the whole of INT 13h
        ; for 82h and 83h, by hooking INT13.386's translation (none if
        ; INT13.386 is not loaded)
        mov     eax,S_INT13_TRANSLATE
        mov     esi,OFFSET FLAT:ld_xlate
        VXDCALL S_HOOK_DEVICE_SERVICE
        jc      lsi_done
        mov     [ld_prev13],esi
lsi_done:
        clc
        ret

lsi_fail:
        mov     esi,OFFSET FLAT:ld_initerr
        xor     eax,eax
        VXDCALL S_FATAL_ERROR_HANDLER

; lsi_cblinit - EBP -> a cable: the DOS programs' view of it starts
; with the master selected, interrupts on, the status ready
lsi_cblinit:
        mov     byte ptr [ebp+CBL_VW+5],0A0h
        mov     byte ptr [ebp+CBL_VR+5],0A0h
        mov     byte ptr [ebp+CBL_VCTL],08h
        mov     byte ptr [ebp+CBL_VST],ST_DRDY OR 10h
        ret

; lsi_cable - EBP -> a cable, SI = its two units' bytes (the master's
; low), EDI -> its two descriptors, EDX -> their names.  A cable with a
; drive of LRGDISK's on it: its interrupt (the second's, from VPICD;
; the first's is BlockDev's), its drives registered, its ports taken.
; EDX changed.
lsi_cable:
        mov     eax,esi
        or      al,ah
        test    al,RF_OURS
        jz      lsc_out                 ; nothing of ours: left alone
        cmp     byte ptr [ebp+CBL_NUM],0
        je      lsc_irqok
        push    edi
        mov     edi,OFFSET FLAT:ld_vid15
        VXDCALL S_VPICD_VIRTUALIZE_IRQ
        pop     edi
        jc      lsc_out                 ; (somebody has IRQ 15: the BIOS
        mov     [ebp+CBL_IRQH],eax      ; keeps the cable)
        VXDCALL S_VPICD_PHYS_UNMASK
lsc_irqok:
        or      byte ptr [ebp+CBL_FLAGS],CF_PRESENT
        xor     ecx,ecx                 ; each unit
lsc_unit:
        test    si,RF_OURS
        jz      lsc_notours
        push    ecx
        push    edx
        push    esi
        mov     bl,byte ptr [esp]       ; its byte
        call    ld_initbdd
        pop     esi
        pop     edx
        pop     ecx
        jc      lsi_fail
        bts     dword ptr [ebp+CBL_OURS],ecx
        jmp     lsc_next
lsc_notours:
        test    si,RF_OTHER             ; something DOS programs drive:
        jz      lsc_next                ; the cable is shared, and their
        or      byte ptr [ebp+CBL_FLAGS],CF_OTHER ; view of it starts
        mov     al,cl                   ; with that unit
        shl     al,4
        or      al,0A0h
        mov     [ebp+CBL_VW+5],al
        mov     [ebp+CBL_VR+5],al
lsc_next:
        shr     si,8                    ; the slave's byte
        add     edi,BP_SIZE
        add     edx,9
        inc     ecx
        cmp     ecx,2
        jb      lsc_unit

        push    edx                     ; the ports
        mov     esi,OFFSET FLAT:ld_iotrap
        movzx   edx,word ptr [ebp+CBL_DATA]
        mov     ecx,8
lsc_port:
        VXDCALL S_INSTALL_IO_HANDLER
        jc      lsi_fail
        inc     edx
        loop    lsc_port
        movzx   edx,word ptr [ebp+CBL_CTRL] ; (not +1: the diskette's
        VXDCALL S_INSTALL_IO_HANDLER    ; too)
        pop     edx
        jc      lsi_fail
lsc_out:
        ret

;---------------------------------------------------------------------
; ld_initbdd - BL = a unit's byte, EDI -> its descriptor, EDX -> its
; name, EBP -> its cable, CL = its unit.  The drive is asked what it
; is (IDENTIFY DEVICE, polled), the descriptor is filled in - the
; geometry a stand-in until ld_devinit has the BIOS's - and registered
; with BlockDev.  CF if refused.
;
; The last sector is the drive's own by LBA - INT 13h can only name
; whole cylinders of the BIOS's made-up geometry, and a partition laid
; out for the drive's real 16 heads (MKDISK's are) runs past the last
; of them; BlockDev refuses anything past BD_MAXSEC, and nothing stops
; LRGDISK reaching it - and the table's by CHS, as WDCTRL has it,
; which ld_devinit works out.
;---------------------------------------------------------------------
ld_initbdd:
        push    ebx
        push    ecx
        push    edx
        mov     byte ptr [edi+BD_MAJOR],3
        mov     byte ptr [edi+BD_MINOR],0Ah
        mov     byte ptr [edi+BD_TYPE],5
        mov     al,bl
        shr     al,RF_DISKSHIFT
        and     al,3
        add     al,80h
        mov     [edi+BD_INT13],al
        mov     dword ptr [edi+BD_FLAGS],BDF_INT13 OR BDF_WRITEABLE OR BDF_SERIAL
        mov     [edi+BD_NAMEPTR],edx
        mov     dword ptr [edi+BD_SECSIZE],512
        mov     dword ptr [edi+BD_SYNCPROC],OFFSET FLAT:ld_sync
        mov     dword ptr [edi+BD_CMDPROC],OFFSET FLAT:ld_command
        mov     dword ptr [edi+BD_HWINTPROC],OFFSET FLAT:ld_hwint
        cmp     byte ptr [ebp+CBL_NUM],0 ; (BlockDev's IRQ 14 is the first
        je      lib_irq                 ; cable's only)
        mov     dword ptr [edi+BD_HWINTPROC],0
lib_irq:
        mov     [edi+BP_CABLE],ebp
        mov     [edi+BP_UNIT],cl
        xor     ah,ah                   ; 0 CHS, 1 LBA, 2 LBA48 as well
        test    bl,RF_LBA
        jz      lib_mode
        inc     ah
        test    bl,RF_LBA48
        jz      lib_mode
        inc     ah
lib_mode:
        mov     [edi+BP_MODE],ah
        mov     byte ptr [edi+BP_CONTROL],08h ; not the table's control
                                        ; byte: its old "no retries" bits
                                        ; 6 and 7 are HOB to a newer drive
        mov     dword ptr [edi+BD_HEADS],16 ; the stand-in geometry
        mov     dword ptr [edi+BD_SPT],63
        mov     dword ptr [edi+BD_CYLS],1024
        mov     dword ptr [edi+BD_MAXSEC],16*63*1024-1
        mov     dword ptr [edi+BD_MAXSEC+4],0
        mov     byte ptr [edi+BP_IHEADS],15
        mov     byte ptr [edi+BP_ISPT],63

        ; the drive's own answers
        mov     byte ptr [edi+BP_XFER],0
        call    ld_identify
        jc      lib_out
        cmp     byte ptr [edi+BP_MODE],0
        je      lib_last
        mov     eax,dword ptr [ld_ident+60*2] ; by LBA every sector the
        and     eax,0FFFFFFFh           ; drive has is in reach: its own
        cmp     byte ptr [edi+BP_MODE],2 ; count; past 2TB the first 2TB
        jne     lib_cap
        mov     eax,dword ptr [ld_ident+100*2]
        cmp     dword ptr [ld_ident+102*2],0
        je      lib_cap
        or      eax,-1
lib_cap:
        or      eax,eax
        jz      lib_last
        dec     eax
        mov     dword ptr [edi+BD_MAXSEC],eax
lib_last:
        test    bl,RF_DMA               ; by the engine: the mode the
        jz      lib_reg                 ; drive has selected (49 bit 8;
        cmp     byte ptr [ld_dmaon],0   ; 88's high byte, Ultra, if 53
        je      lib_reg                 ; bit 2; else 63's, multiword,
        cmp     word ptr [ebp+CBL_BM],0 ; if 53 bit 1)
        je      lib_reg
        test    byte ptr [ld_ident+49*2+1],1
        jz      lib_reg
        test    byte ptr [ld_ident+53*2],4
        jz      lib_mw
        mov     al,byte ptr [ld_ident+88*2+1]
        and     al,7Fh
        jz      lib_mw
        call    ld_topbit
        or      al,40h
        mov     [edi+BP_XFER],al
        jmp     lib_reg
lib_mw:
        test    byte ptr [ld_ident+53*2],2
        jz      lib_reg
        mov     al,byte ptr [ld_ident+63*2+1]
        and     al,07h
        jz      lib_reg
        call    ld_topbit
        or      al,20h
        mov     [edi+BP_XFER],al
lib_reg:
        ; By PIO, in blocks.  The size is the one the drive is set to
        ; (word 59, bit 8 saying it is set to one), which is the
        ; BIOS's and is kept; for a drive set to none, the highest
        ; power of two it takes (word 47), LD_BLOCKMAX at most - as
        ; the real-mode start-up worked it out, and said.  Either way
        ; the drive is then TOLD the size, and it is the drive's
        ; taking the command that counts: nothing changes in a drive
        ; that reported right, and one that did not (QEMU's goes on
        ; saying what it said the first time it was asked) is as
        ; LRGDISK takes it to be.  A drive set to blocks of one sector
        ; is left so.
        mov     byte ptr [edi+BP_MULT],0
        cmp     byte ptr [ld_blockon],0
        je      lib_noblock
        xor     al,al
        test    byte ptr [ld_ident+59*2+1],1
        jz      lib_sized
        mov     al,byte ptr [ld_ident+59*2]
lib_sized:
        cmp     al,1
        je      lib_noblock
        ja      lib_tell
        mov     al,byte ptr [ld_ident+47*2]
        cmp     al,2
        jb      lib_noblock
        mov     ah,LD_BLOCKMAX
lib_fit:
        cmp     ah,al
        jbe     lib_fits
        shr     ah,1
        jmp     lib_fit
lib_fits:
        mov     al,ah
lib_tell:
        mov     cl,[edi+BP_UNIT]
        call    ld_setmult
        jc      lib_noblock
        mov     [edi+BP_MULT],al
lib_noblock:
        ; and its data a doubleword at a time, where the drive says
        ; it can (word 48, of ATA-1 and ATA-2) or the controller is a
        ; PCI one - if the IDENTIFY block comes the same that way
        mov     byte ptr [edi+BP_IO32],0
        cmp     byte ptr [ld_io32on],0
        je      lib_no32
        test    byte ptr [ld_ident+48*2],1
        jnz     lib_try32
        cmp     byte ptr [ld_pciide],0
        je      lib_no32
lib_try32:
        mov     cl,[edi+BP_UNIT]
        call    ld_prove32
        jc      lib_no32
        mov     byte ptr [edi+BP_IO32],1
lib_no32:
IFDEF LRG_TRACE
        TRC     42h             ; B: 32-bit data, and the block's size
        movzx   eax,word ptr [edi+BP_MULT]
        TRCHEX  eax, 4
        TRC     13
        TRC     10
ENDIF
        VXDCALL S_BD_REGISTER_DEVICE
lib_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

; ld_topbit - AL = a byte with bits set -> AL = its highest set bit
ld_topbit:
        push    ecx
        xor     cl,cl
ltb_next:
        shr     al,1
        jz      ltb_done
        inc     cl
        jmp     ltb_next
ltb_done:
        mov     al,cl
        pop     ecx
        ret

;---------------------------------------------------------------------
; ld_devinit - Device_Init: EBX = the System VM.  Each registered
; drive is given the BIOS's geometry: INT 13h AH=08h, run in the
; System VM and let through to the BIOS by LRGDISK's own hook on
; INT13.386's translation (ld_rawbios), which would otherwise answer
; it from the descriptor.  By CHS the last sector is the table's
; cylinders' (disks 80h and 81h), else the highest plus 1.
;---------------------------------------------------------------------
ld_devinit:
        mov     edi,OFFSET FLAT:ld_bdd
ldv_unit:
        cmp     byte ptr [edi+BD_INT13],0
        je      ldv_next
        mov     dl,[edi+BD_INT13]
        call    ld_biosgeo
        jc      ldv_next
        movzx   eax,byte ptr [ld_geo+2]
        mov     [edi+BD_SPT],eax
        mov     [edi+BP_ISPT],al
        movzx   eax,word ptr [ld_geo]
        mov     [edi+BD_HEADS],eax
        dec     al
        mov     [edi+BP_IHEADS],al
        movzx   ecx,word ptr [ld_geo+3] ; the highest cylinder
        lea     eax,[ecx+2]
        mov     [edi+BD_CYLS],eax
        cmp     byte ptr [edi+BP_MODE],0
        jne     ldv_next                ; (by LBA the drive's own count)
        inc     ecx                     ; the last sector by CHS: the
        cmp     byte ptr [edi+BD_INT13],81h ; table's cylinders if there
        ja      ldv_cyls                ; is a table, else the highest
        mov     eax,41h*4               ; plus 1
        cmp     byte ptr [edi+BD_INT13],80h
        je      ldv_table
        mov     eax,46h*4
ldv_table:
        add     eax,[ebx+CB_HIGHLIN]
        mov     eax,[eax]               ; the vector: segment:offset
        mov     edx,eax
        shr     edx,16
        shl     edx,4
        movzx   eax,ax
        add     eax,edx
        add     eax,[ebx+CB_HIGHLIN]
        movzx   ecx,word ptr [eax]
ldv_cyls:
        imul    ecx,[edi+BD_HEADS]
        imul    ecx,[edi+BD_SPT]
        dec     ecx
        mov     dword ptr [edi+BD_MAXSEC],ecx
ldv_next:
        add     edi,BP_SIZE
        cmp     edi,OFFSET FLAT:ld_bdd+4*BP_SIZE
        jb      ldv_unit
        clc
        ret

;---------------------------------------------------------------------
; ld_biosgeo - DL = a BIOS disk, EBX = the System VM: INT 13h AH=08h,
; of the BIOS, into ld_geo (heads, sectors a track, the highest
; cylinder).  CF if the BIOS refused.  EAX, ECX, EDX changed.
;---------------------------------------------------------------------
ld_biosgeo:
        push    ebp
        push    edx
        VXDCALL S_BEGIN_NEST_V86_EXEC
        pop     edx
        mov     ebp,[ebx+CB_CLIENTPTR]
        mov     byte ptr [ebp+CL_AH],08h
        mov     [ebp+CL_DL],dl
        mov     byte ptr [ld_rawbios],1
        mov     eax,13h
        VXDCALL S_EXEC_INT
        mov     byte ptr [ld_rawbios],0
        mov     eax,[ebp+CL_EFLAGS]
        mov     cx,[ebp+CL_CX]
        mov     dh,[ebp+CL_DH]
        VXDCALL S_END_NEST_EXEC
        pop     ebp
        test    al,1
        jnz     lbg_no
        mov     al,cl
        and     al,3Fh
        jz      lbg_no
        mov     byte ptr [ld_geo+2],al
        mov     al,cl
        shr     al,6
        mov     byte ptr [ld_geo+4],al
        mov     byte ptr [ld_geo+3],ch
        movzx   ax,dh
        inc     ax
        mov     [ld_geo],ax
        clc
        ret
lbg_no:
        stc
        ret

;---------------------------------------------------------------------
; ld_identify - CL = a unit of cable EBP: IDENTIFY DEVICE into
; ld_ident, asked with the drive's interrupt off (nIEN) and polled.
; CF if the drive did not answer.  EAX, EDX changed.
;---------------------------------------------------------------------
ld_identify:
        push    ecx
        push    edi
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,0Ah
        out     dx,al
        mov     al,cl
        shl     al,4
        or      al,0A0h
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_DRVHD
        out     dx,al
        call    ld_pollbsy
        jc      lid_none
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_CMD
        mov     al,ATA_IDENTIFY
        out     dx,al
        call    ld_delay
        call    ld_pollbsy
        jc      lid_none
        test    al,ST_ERR
        jnz     lid_none
        test    al,ST_DRQ
        jz      lid_none
        mov     edi,OFFSET FLAT:ld_ident
        mov     ecx,256
        movzx   edx,word ptr [ebp+CBL_DATA]
        cld
        rep     insw
        clc
        jmp     lid_out
lid_none:
        stc
lid_out:
        pushfd
        movzx   edx,word ptr [ebp+CBL_DATA] ; nothing left pending, and
        add     edx,R_STATUS            ; the interrupt back on
        in      al,dx
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,08h
        out     dx,al
        popfd
        pop     edi
        pop     ecx
        ret

;---------------------------------------------------------------------
; ld_setmult - CL = a unit of cable EBP, AL = sectors a block: SET
; MULTIPLE MODE, with the drive's interrupt off (nIEN) and polled.
; CF if the drive refuses.  EDX changed.
;---------------------------------------------------------------------
ld_setmult:
        push    eax
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,0Ah
        out     dx,al
        mov     al,cl
        shl     al,4
        or      al,0A0h
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_DRVHD
        out     dx,al
        call    ld_pollbsy
        jc      lsm_no
        mov     al,byte ptr [esp]
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_COUNT
        out     dx,al
        add     edx,R_CMD-R_COUNT
        mov     al,ATA_SETMULT
        out     dx,al
        call    ld_delay
        call    ld_pollbsy
        jc      lsm_no
        test    al,ST_ERR
        jnz     lsm_no
        clc
        jmp     lsm_out
lsm_no:
        stc
lsm_out:
        pushfd
        movzx   edx,word ptr [ebp+CBL_DATA] ; nothing left pending, and
        add     edx,R_STATUS            ; the interrupt back on
        in      al,dx
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,08h
        out     dx,al
        popfd
        pop     eax
        ret

;---------------------------------------------------------------------
; ld_prove32 - CL = a unit of cable EBP, ld_ident = its IDENTIFY block
; as it came a word at a time: the block asked for again and taken a
; doubleword at a time, into ld_id32.  CF unless the two are the same
; 512 bytes and the drive had nothing more to give.
;
; A host that cannot latch two words reports no error.  It reads half
; the block - each doubleword a word of the data and a word of
; whatever is at the next port - and leaves the drive holding the
; other half out; that is taken and dropped, or the next command
; would be written to a drive still waiting to be read.  The kernel's
; ata_prove32 (IO\ATA.INC) is the same measurement and says more.
; EAX, EDX changed.
;---------------------------------------------------------------------
ld_prove32:
        push    ecx
        push    esi
        push    edi
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,0Ah
        out     dx,al
        mov     al,cl
        shl     al,4
        or      al,0A0h
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_DRVHD
        out     dx,al
        call    ld_pollbsy
        jc      l32_no
        movzx   edx,word ptr [ebp+CBL_DATA]
        add     edx,R_CMD
        mov     al,ATA_IDENTIFY
        out     dx,al
        call    ld_delay
        call    ld_pollbsy
        jc      l32_no
        test    al,ST_ERR
        jnz     l32_no
        test    al,ST_DRQ
        jz      l32_no
        mov     edi,OFFSET FLAT:ld_id32
        mov     ecx,128
        movzx   edx,word ptr [ebp+CBL_DATA]
        cld
        rep     insd
        call    ld_delay
        xor     esi,esi                 ; ESI = words it still had
        mov     ecx,256                 ; (no block has more)
l32_drain:
        movzx   edx,word ptr [ebp+CBL_CTRL]
        in      al,dx
        test    al,ST_BSY
        jnz     l32_next
        test    al,ST_DRQ
        jz      l32_same
        movzx   edx,word ptr [ebp+CBL_DATA]
        in      ax,dx
        inc     esi
l32_next:
        dec     ecx
        jnz     l32_drain
l32_same:
        or      esi,esi
        jnz     l32_no
        mov     esi,OFFSET FLAT:ld_ident
        mov     edi,OFFSET FLAT:ld_id32
        mov     ecx,128
        repe    cmpsd
        jne     l32_no
        clc
        jmp     l32_out
l32_no:
        stc
l32_out:
        pushfd
        movzx   edx,word ptr [ebp+CBL_DATA] ; nothing left pending, and
        add     edx,R_STATUS            ; the interrupt back on
        in      al,dx
        movzx   edx,word ptr [ebp+CBL_CTRL]
        mov     al,08h
        out     dx,al
        popfd
        pop     edi
        pop     esi
        pop     ecx
        ret

;---------------------------------------------------------------------
; ld_pollbsy - BSY gone within four million reads of the alternate
; status, a few seconds on any bus.  AL = status, CF if it stayed;
; EDX changed.
;---------------------------------------------------------------------
ld_pollbsy:
        push    ecx
        mov     ecx,4000000
        movzx   edx,word ptr [ebp+CBL_CTRL]
lpb_loop:
        in      al,dx
        test    al,ST_BSY
        jz      lpb_ok
        loop    lpb_loop
        stc
        pop     ecx
        ret
lpb_ok:
        clc
        pop     ecx
        ret

;---------------------------------------------------------------------
; ld_findbm - the PCI IDE controller's bus-master engine, through
; configuration mechanism 1 (ports CF8h and CFCh): the first device of
; class 01h 01h on the first eight buses, its BAR4 (the engine's
; sixteen ports) and its programming interface (bits 0 and 2: a cable
; in native mode is not at 1F0h or 170h, so its eight are not for
; that cable), and it is told it may master the bus.  ld_bmbase and
; the cables' CBL_BM; CF if there is none.  EAX, EBX, ECX, EDX
; changed.
;---------------------------------------------------------------------
ld_findbm:
        xor     ebx,ebx                 ; bus, device and function in
lfb_dev:                                ; bits 16-23, 11-15, 8-10
        xor     al,al
        call    ld_pciread
        cmp     ax,0FFFFh               ; no device
        je      lfb_next
        mov     al,08h
        call    ld_pciread
        shr     eax,16
        cmp     ax,0101h
        je      lfb_found
lfb_next:
        add     ebx,100h
        cmp     ebx,00080000h           ; eight buses
        jb      lfb_dev
        stc
        ret

lfb_found:
        mov     al,20h                  ; BAR4
        call    ld_pciread
        test    al,1
        jz      lfb_next
        and     eax,0FFFCh
        jz      lfb_next
        mov     [ld_bmbase],ax
        mov     ecx,eax
        mov     al,08h
        call    ld_pciread              ; the programming interface
        test    ah,1
        jnz     lfb_second
        mov     word ptr [ld_cbl0+CBL_BM],cx
lfb_second:
        test    ah,4
        jnz     lfb_cmd
        add     ecx,8
        mov     word ptr [ld_cbl1+CBL_BM],cx
lfb_cmd:
        mov     al,04h                  ; the command register: I/O and
        call    ld_pciread              ; bus mastering on
        or      eax,PCIC_IOEN OR PCIC_BUSMASTER
        mov     ecx,eax
        mov     al,04h
        call    ld_pciwrite
        clc
        ret

; ld_pciread - EBX = the device, AL = a register -> EAX = its dword.
; ld_pciwrite - ...<- ECX.  Nothing else changed.
ld_pciread:
        push    edx
        pushfd
        cli
        and     eax,0FCh
        or      eax,ebx
        or      eax,80000000h
        mov     edx,PCI_CFG_ADDR
        out     dx,eax
        mov     edx,PCI_CFG_DATA
        in      eax,dx
        popfd
        pop     edx
        ret

ld_pciwrite:
        push    edx
        pushfd
        cli
        and     eax,0FCh
        or      eax,ebx
        or      eax,80000000h
        mov     edx,PCI_CFG_ADDR
        out     dx,eax
        mov     edx,PCI_CFG_DATA
        mov     eax,ecx
        out     dx,eax
        popfd
        pop     edx
        ret
_ITEXT  ENDS

;=====================================================================
; START-UP, REAL MODE
;
; Run by WIN386's loader before Windows leaves real mode: AX = the
; VMM's version, BX = the loader's flags, ECX = its service entry
; point, SI = the environment.  DS is this segment.  Returns AX =
; 0 to load the protected-mode side or 1 not to (8000h set: the
; loader says nothing), BX = SI = 0 (no pages to exclude, no instance
; data), and EDX = the reference data: a byte a unit (RF_*), the first
; cable's master in the low byte.  Nothing more can be handed over -
; the loader owns every kind of memory by now, and this segment is
; reused for the next device's start-up - so the protected-mode side
; measures the geometry and asks the drives again (ld_initbdd).
;
; NO ADDRESS IN HERE IS NAMED DIRECTLY - see the top of the file.
;=====================================================================
_RCODE  SEGMENT
        ASSUME  cs:_RCODE, ds:_RCODE, es:_RCODE, ss:NOTHING
rbase   LABEL   BYTE

RV_TEXTMAX      EQU     3200            ; the report's buffer

rm_entry:
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es
        cld
        sti
        mov     dword ptr ds:[rv_svc-rbase],ecx
        mov     word ptr ds:[rv_tlen-rbase],0
        mov     si,m_hello-rbase
        call    rm_puts
        cmp     ax,030Ah
        jae     rmi_ver
        mov     si,m_oldwin-rbase
        jmp     rmi_refuse
rmi_ver:
        test    bx,0003h                ; another of us already loaded
        jnz     rmi_quiet

        ; SYSTEM.INI: 32BitDiskAccess (on unless it says off), whether
        ; to leave a report, and whether the engine may be used
        mov     di,k_32bit-rbase
        mov     ecx,1
        call    rm_profile
        or      cx,cx
        jz      rmi_quiet
        mov     di,k_report-rbase
        xor     ecx,ecx
        call    rm_profile
        mov     byte ptr ds:[rv_report-rbase],cl
        or      byte ptr ds:[rv_report-rbase],ch
        mov     di,k_dma-rbase
        mov     ecx,1
        call    rm_profile
        mov     byte ptr ds:[rv_dma-rbase],cl
        or      byte ptr ds:[rv_dma-rbase],ch
        mov     di,k_serial-rbase
        xor     ecx,ecx
        call    rm_profile
        mov     byte ptr ds:[rv_serial-rbase],cl
        or      byte ptr ds:[rv_serial-rbase],ch
        mov     di,k_block-rbase        ; and the three that say how PIO
        mov     ecx,1                   ; goes, which the protected-mode
        call    rm_profile              ; side reads for itself
        mov     byte ptr ds:[rv_block-rbase],cl
        or      byte ptr ds:[rv_block-rbase],ch
        mov     di,k_poll-rbase
        mov     ecx,1
        call    rm_profile
        mov     byte ptr ds:[rv_poll-rbase],cl
        or      byte ptr ds:[rv_poll-rbase],ch
        mov     di,k_io32-rbase
        mov     ecx,1
        call    rm_profile
        mov     byte ptr ds:[rv_io32-rbase],cl
        or      byte ptr ds:[rv_io32-rbase],ch

        ; how many hard disks, through DOS's INT 13h as anyone asks
        mov     ah,08h
        mov     dl,80h
        int     13h
        sti
        push    cs
        pop     es
        jc      rmi_nodisk
        or      dl,dl
        jz      rmi_nodisk
        cmp     dl,4
        jbe     rmi_ndrv
        mov     dl,4
rmi_ndrv:
        mov     byte ptr ds:[rv_ndrv-rbase],dl

        ; As WDCTRL does: tell the caches hardware is being probed,
        ; have DOS write what it holds, raise InDOS so that no TSR pops
        ; up into the middle of it, and give anything in flight two
        ; timer ticks to finish
        mov     ax,1607h
        mov     bx,0010h                ; BlockDev's number
        mov     cx,1                    ; detection starts
        int     2Fh
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     ah,0Dh
        int     21h
        mov     ah,34h
        int     21h
        inc     byte ptr es:[bx]
        mov     word ptr ds:[rv_indos-rbase],bx
        mov     word ptr ds:[rv_indos+2-rbase],es
        mov     byte ptr ds:[rv_detect-rbase],1
        push    cs
        pop     es
        call    rm_settle

        ; INT 2Fh AH=13h swaps in a handler for DOS's disk driver and
        ; hands back what was there: what DOS calls (DS:DX) and the
        ; ROM's own entry (ES:BX).  Swapped straight back.
        mov     ah,13h
        mov     dx,rm_hang-rbase
        mov     bx,dx
        int     2Fh
        mov     word ptr cs:[rv_dos13-rbase],dx
        mov     word ptr cs:[rv_dos13+2-rbase],ds
        mov     word ptr cs:[rv_rom13-rbase],bx
        mov     word ptr cs:[rv_rom13+2-rbase],es
        mov     ah,13h
        int     2Fh
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     ax,cs
        cmp     ax,word ptr ds:[rv_dos13+2-rbase]
        jne     rmi_2fok
        mov     si,m_no2f-rbase
        jmp     rmi_refuse
rmi_2fok:
        ; Anything between DOS and a ROM entry below A000h is a disk
        ; program of some kind - a drive overlay that moves every
        ; sector, perhaps - unless it says it knows about BlockDev
        cmp     word ptr ds:[rv_rom13+2-rbase],0A000h
        jae     rmi_chain
        mov     ax,1607h
        mov     bx,0010h
        mov     cx,3                    ; the INT 13h chain check
        int     2Fh
        push    cs
        pop     ds
        push    cs
        pop     es
        jcxz    rmi_chain
        mov     si,m_hooked-rbase
        jmp     rmi_refuse
rmi_chain:

        ; the bus-master engine, if there is one, and the two chips
        ; with bugs
        call    rm_findbm
        call    rm_chipbugs
        cmp     byte ptr ds:[rv_poll-rbase],0
        jne     rmi_polls
        mov     si,m_poll_off-rbase
        call    rm_puts
rmi_polls:

        ; Each BIOS disk: which cable and unit it is, if it is on one
        ; at all, and whether LRGDISK takes it
        mov     bl,80h
rmi_disk:
        push    bx
        call    rm_check
        pop     bx
        inc     bl
        mov     al,bl
        sub     al,80h
        cmp     al,byte ptr ds:[rv_ndrv-rbase]
        jb      rmi_disk

        ; and anything else on a cable LRGDISK has a drive on, which
        ; DOS programs go on driving: LRGDISK shares the cable with them
        xor     bx,bx                   ; BL = unit, BH = cable
rmi_unit:
        movzx   si,bh
        test    byte ptr ds:[si+(rv_cbl-rbase)],1
        jz      rmi_unitok
        mov     al,bh
        call    rm_setcable
        mov     byte ptr ds:[rc_unit-rbase],bl
        call    rm_unitrec
        test    byte ptr ds:[di+UR_FLAGS],RF_OURS
        jnz     rmi_unitok
        mov     al,bl
        call    rm_present
        jc      rmi_unitok
        call    rm_unitrec
        or      byte ptr ds:[di+UR_FLAGS],RF_OTHER
        mov     si,m_other-rbase
        call    rm_puts
        mov     al,bl
        add     al,'0'
        call    rm_putc
        call    rm_cablename
        mov     si,m_other_ata-rbase
        cmp     word ptr ds:[rv_pst+2-rbase],14EBh
        je      rmi_atapi
        cmp     word ptr ds:[rv_pst+2-rbase],6996h
        jne     rmi_kind
rmi_atapi:
        mov     si,m_other_atapi-rbase
rmi_kind:
        call    rm_puts
        mov     eax,dword ptr ds:[rv_pst-rbase]
        mov     cl,8
        call    rm_hex
        mov     si,m_other2-rbase
        call    rm_puts
rmi_unitok:
        inc     bl
        cmp     bl,2
        jb      rmi_unit
        xor     bl,bl
        inc     bh
        cmp     bh,2
        jb      rmi_unit

        mov     al,byte ptr ds:[rv_cbl-rbase]
        or      al,byte ptr ds:[rv_cbl+1-rbase]
        test    al,1
        jnz     rmi_load
        mov     si,m_none-rbase
        jmp     rmi_refuse

rmi_load:
        ; the answer: a byte a unit, and the whole-driver bits
        mov     al,byte ptr ds:[rv_units+0*UR_SIZE+UR_FLAGS-rbase]
        mov     ah,byte ptr ds:[rv_units+1*UR_SIZE+UR_FLAGS-rbase]
        shl     eax,16
        mov     al,byte ptr ds:[rv_units+2*UR_SIZE+UR_FLAGS-rbase]
        mov     ah,byte ptr ds:[rv_units+3*UR_SIZE+UR_FLAGS-rbase]
        rol     eax,16
        cmp     byte ptr ds:[rv_serial-rbase],0
        je      rmi_noserial
        or      al,RF_SERIAL
rmi_noserial:
        cmp     byte ptr ds:[rv_dma-rbase],0
        je      rmi_nodma
        or      ah,RF_DMAON
rmi_nodma:
        cmp     byte ptr ds:[rv_pciide-rbase],0
        je      rmi_nopci
        or      eax,RF_PCIIDE SHL 16
rmi_nopci:
        cmp     byte ptr ds:[rv_no32-rbase],0
        je      rmi_can32
        or      eax,RF_NO32 SHL 24
rmi_can32:
        mov     dword ptr ds:[rv_answer-rbase],eax
        mov     si,m_on-rbase
        call    rm_puts
        call    rm_cleanup
        cmp     byte ptr ds:[rv_report-rbase],0
        je      rmi_ok
        call    rm_show
        call    rm_file
rmi_ok:
        mov     edx,dword ptr ds:[rv_answer-rbase]
        xor     ax,ax                   ; Device_Load_Ok
        jmp     rmi_out

rmi_nodisk:
        mov     si,m_nodisk-rbase
rmi_refuse:
        call    rm_puts
        mov     si,m_off-rbase
        call    rm_puts
        call    rm_cleanup
        call    rm_show
        cmp     byte ptr ds:[rv_report-rbase],0
        je      rmi_quiet
        call    rm_file
rmi_quiet:
        call    rm_cleanup
        xor     edx,edx
        mov     ax,8001h                ; Abort_Device_Load, no message
rmi_out:
        xor     bx,bx
        xor     si,si
        pop     es
        pop     ds
        ret

;---------------------------------------------------------------------
; rm_check - one BIOS disk, BL = 80h to 83h: which cable and unit it
; is, whether LRGDISK takes it, and how.  Its unit record goes to
; rv_units; a line for the report either way.
;---------------------------------------------------------------------
rm_check:
        push    bx
        mov     byte ptr ds:[rc_drive-rbase],bl
        mov     byte ptr ds:[rc_mode-rbase],0FFh
        mov     byte ptr ds:[rc_xfer-rbase],0
        mov     byte ptr ds:[rc_48-rbase],0
        mov     dword ptr ds:[rc_val-rbase],0
        xor     al,al
        call    rm_setcable
        mov     si,m_disk-rbase
        call    rm_puts
        movzx   eax,bl
        mov     cl,2
        call    rm_hex
        mov     si,m_disk2-rbase
        call    rm_puts

        ; 1: the BIOS's geometry
        mov     byte ptr ds:[rc_step-rbase],1
        mov     ah,08h
        mov     dl,bl
        call    rm_rom13
        jnc     rc_08
        mov     byte ptr ds:[rc_val-rbase],ah
        jmp     rc_fail
rc_08:
        mov     al,cl
        and     al,3Fh
        mov     byte ptr ds:[rc_spt-rbase],al
        mov     ah,cl
        shr     ah,6
        mov     al,ch
        mov     word ptr ds:[rc_maxcyl-rbase],ax
        movzx   ax,dh
        inc     ax
        mov     word ptr ds:[rc_heads-rbase],ax
        cmp     byte ptr ds:[rc_spt-rbase],0
        je      rc_fail

        ; the geometry, for the report
        movzx   eax,word ptr ds:[rc_maxcyl-rbase]
        inc     ax
        call    rm_dec
        mov     al,'/'
        call    rm_putc
        mov     ax,word ptr ds:[rc_heads-rbase]
        call    rm_dec
        mov     al,'/'
        call    rm_putc
        movzx   ax,byte ptr ds:[rc_spt-rbase]
        call    rm_dec

        ; 2-4: the table INT 41h or INT 46h names, for disks 80h and
        ; 81h: the same heads and sectors, and its cylinders 1 to 3
        ; past the highest.  82h and 83h have no table: the highest
        ; plus 1 stands in for its cylinders.
        mov     ax,word ptr ds:[rc_maxcyl-rbase]
        inc     ax
        mov     word ptr ds:[rc_tcyl-rbase],ax
        cmp     bl,81h
        ja      rc_notable
        xor     ax,ax
        mov     es,ax
        mov     di,41h*4
        cmp     bl,80h
        je      rc_table
        mov     di,46h*4
rc_table:
        les     di,dword ptr es:[di]
        mov     byte ptr ds:[rc_step-rbase],2
        movzx   ax,byte ptr es:[di+2]
        mov     word ptr ds:[rc_val-rbase],ax
        cmp     ax,word ptr ds:[rc_heads-rbase]
        jne     rc_failes
        mov     byte ptr ds:[rc_step-rbase],3
        mov     al,byte ptr es:[di+0Eh]
        mov     byte ptr ds:[rc_val-rbase],al
        cmp     al,byte ptr ds:[rc_spt-rbase]
        jne     rc_failes
        mov     byte ptr ds:[rc_step-rbase],4
        mov     ax,word ptr es:[di]
        mov     word ptr ds:[rc_val-rbase],ax
        mov     word ptr ds:[rc_tcyl-rbase],ax
        sub     ax,word ptr ds:[rc_maxcyl-rbase]
        jb      rc_failes
        cmp     ax,1
        jb      rc_failes
        cmp     ax,3
        ja      rc_failes
        push    cs
        pop     es
        mov     si,m_table-rbase
        call    rm_puts
        mov     ax,word ptr ds:[rc_tcyl-rbase]
        call    rm_dec
        jmp     rc_tabled
rc_notable:
        mov     si,m_notable-rbase
        call    rm_puts
rc_tabled:
        mov     si,m_crlf-rbase
        call    rm_puts

        ; 5: which cable and unit it is - both cables' registers
        ; poisoned (rm_poison2), the BIOS's read of its first sector,
        ; and the cable whose registers show the read (rm_whichcable)
        ; - and not a unit another BIOS disk turned out to be
        mov     byte ptr ds:[rc_step-rbase],5
        mov     dword ptr ds:[rc_t-rbase],0
        call    rm_poison2
        call    rm_biosread
        jnc     rc_unitread
        mov     byte ptr ds:[rc_val-rbase],ah
        jmp     rc_fail
rc_unitread:
        call    rm_whichcable
        jc      rc_fail
        call    rm_unitrec              ; DI -> its record
        test    byte ptr ds:[di+UR_FLAGS],RF_OURS
        jnz     rc_fail

        ; 6: the cable's interrupt reaches the processor (rm_irqtest):
        ; a BIOS that polls may have left it masked, as SeaBIOS leaves
        ; IRQ 15; one that does not arrive at all would leave every
        ; command of LRGDISK's waiting
        mov     byte ptr ds:[rc_step-rbase],6
        call    rm_irqtest
        jc      rc_fail

        ; 7: the drive itself
        mov     byte ptr ds:[rc_step-rbase],7
        mov     al,byte ptr ds:[rc_unit-rbase]
        call    rm_identify
        jc      rc_fail

        ; For PIO: the most sectors it moves as a block (word 47), the
        ; block's size it is set to now (word 59, if bit 8), and
        ; whether it says its data may be taken a doubleword at a
        ; time (word 48, which ATA-3 dropped)
        mov     al,byte ptr ds:[rv_buf2+47*2-rbase]
        mov     byte ptr ds:[rc_mmax-rbase],al
        xor     al,al
        test    byte ptr ds:[rv_buf2+59*2+1-rbase],1
        jz      rc_nomult
        mov     al,byte ptr ds:[rv_buf2+59*2-rbase]
rc_nomult:
        mov     byte ptr ds:[rc_mcur-rbase],al
        mov     al,byte ptr ds:[rv_buf2+48*2-rbase]
        and     al,1
        mov     byte ptr ds:[rc_w48-rbase],al

        ; What the drive says of itself: LBA (word 49 bit 9), its LBA
        ; sector count (60-61, 28 bits; with LBA48 supported and on,
        ; words 83 and 86 bit 10, 100-103), its own geometry (3 and 6),
        ; the one the BIOS has set it to (55 and 56, if word 53 bit 0),
        ; and the DMA mode selected on it, if any (49 bit 8; 88's high
        ; byte, Ultra, if 53 bit 2; else 63's, multiword, if 53 bit 1).
        ; Past 2TB, the first 2TB: a sector number in DOS, and in a
        ; partition table, is 32 bits.
        mov     ax,word ptr ds:[rv_buf2+49*2-rbase]
        shr     ax,9
        and     al,1
        mov     byte ptr ds:[rc_lbaok-rbase],al
        mov     eax,dword ptr ds:[rv_buf2+60*2-rbase]
        and     eax,0FFFFFFFh
        mov     dword ptr ds:[rc_cap-rbase],eax
        test    byte ptr ds:[rv_buf2+83*2+1-rbase],04h
        jz      rc_no48
        test    byte ptr ds:[rv_buf2+86*2+1-rbase],04h
        jz      rc_no48
        mov     eax,dword ptr ds:[rv_buf2+100*2-rbase]
        cmp     dword ptr ds:[rv_buf2+102*2-rbase],0
        je      rc_48cap
        or      eax,-1
rc_48cap:
        cmp     eax,dword ptr ds:[rc_cap-rbase]
        jbe     rc_no48
        mov     dword ptr ds:[rc_cap-rbase],eax
        mov     byte ptr ds:[rc_48-rbase],1
rc_no48:
        mov     ax,word ptr ds:[rv_buf2+3*2-rbase]
        mov     word ptr ds:[rc_dheads-rbase],ax
        mov     word ptr ds:[rc_cheads-rbase],ax
        mov     ax,word ptr ds:[rv_buf2+6*2-rbase]
        mov     word ptr ds:[rc_dspt-rbase],ax
        mov     word ptr ds:[rc_cspt-rbase],ax
        test    byte ptr ds:[rv_buf2+53*2-rbase],1
        jz      rc_nocur
        mov     ax,word ptr ds:[rv_buf2+55*2-rbase]
        mov     word ptr ds:[rc_cheads-rbase],ax
        mov     ax,word ptr ds:[rv_buf2+56*2-rbase]
        mov     word ptr ds:[rc_cspt-rbase],ax
rc_nocur:
        test    byte ptr ds:[rv_buf2+49*2+1-rbase],1 ; DMA at all?
        jz      rc_nodma
        test    byte ptr ds:[rv_buf2+53*2-rbase],4
        jz      rc_nomw
        mov     al,byte ptr ds:[rv_buf2+88*2+1-rbase]
        and     al,7Fh
        jz      rc_nomw
        call    rm_topbit
        or      al,40h
        mov     byte ptr ds:[rc_xfer-rbase],al
        jmp     rc_nodma
rc_nomw:
        test    byte ptr ds:[rv_buf2+53*2-rbase],2
        jz      rc_nodma
        mov     al,byte ptr ds:[rv_buf2+63*2+1-rbase]
        and     al,07h
        jz      rc_nodma
        call    rm_topbit
        or      al,20h
        mov     byte ptr ds:[rc_xfer-rbase],al
rc_nodma:
        mov     si,m_drive-rbase
        call    rm_puts
        mov     al,byte ptr ds:[rc_unit-rbase]
        add     al,'0'
        call    rm_putc
        call    rm_cablename
        mov     si,m_drive2-rbase
        call    rm_puts
        mov     eax,dword ptr ds:[rc_cap-rbase]
        call    rm_dec32
        mov     si,m_nolba-rbase
        cmp     byte ptr ds:[rc_lbaok-rbase],0
        je      rc_lbasaid
        mov     si,m_haslba-rbase
        cmp     byte ptr ds:[rc_48-rbase],0
        je      rc_lbasaid
        mov     si,m_has48-rbase
rc_lbasaid:
        call    rm_puts

        ; and how it will be moved: by the engine, if the drive has a
        ; mode selected and the cable an engine
        mov     word ptr ds:[rc_dmamsg-rbase],m_dma_off-rbase
        cmp     byte ptr ds:[rv_dma-rbase],0
        je      rc_dmasaid
        mov     word ptr ds:[rc_dmamsg-rbase],m_dma_none-rbase
        cmp     byte ptr ds:[rc_xfer-rbase],0
        je      rc_dmasaid
        mov     word ptr ds:[rc_dmamsg-rbase],m_dma_noeng-rbase
        movzx   bx,byte ptr ds:[rc_cable-rbase]
        add     bx,bx
        cmp     word ptr ds:[bx+(rv_bm-rbase)],0
        je      rc_dmasaid
        mov     word ptr ds:[rc_dmamsg-rbase],m_dma_udma-rbase
        test    byte ptr ds:[rc_xfer-rbase],40h
        jnz     rc_dmasaid
        mov     word ptr ds:[rc_dmamsg-rbase],m_dma_mw-rbase
rc_dmasaid:

        ; the sectors the BIOS reaches: (highest+1) x heads x sectors
        movzx   eax,word ptr ds:[rc_maxcyl-rbase]
        inc     eax
        movzx   ecx,word ptr ds:[rc_heads-rbase]
        imul    eax,ecx
        movzx   ecx,byte ptr ds:[rc_spt-rbase]
        imul    eax,ecx
        mov     dword ptr ds:[rc_total-rbase],eax

        ; Four of them: the first; cylinder 1, head 1, sector 3, where a
        ; wrong head or cylinder count shows; most of the way in; the
        ; last.  (rm_clamp keeps them inside a small disk.)
        mov     dword ptr ds:[rc_list-rbase],0
        movzx   eax,word ptr ds:[rc_heads-rbase]
        inc     eax
        movzx   ecx,byte ptr ds:[rc_spt-rbase]
        imul    eax,ecx
        add     eax,2
        mov     dword ptr ds:[rc_list+4-rbase],eax
        mov     eax,dword ptr ds:[rc_total-rbase]
        shr     eax,3
        imul    eax,eax,7
        add     eax,5
        mov     dword ptr ds:[rc_list+8-rbase],eax
        mov     eax,dword ptr ds:[rc_total-rbase]
        dec     eax
        mov     dword ptr ds:[rc_list+12-rbase],eax

        ; 8-11: how the BIOS addresses the drive, from what each of its
        ; reads leaves in the registers - narrowed read by read
        mov     byte ptr ds:[rc_how-rbase],HOW_ALL
        xor     bx,bx
rc_pass1:
        mov     eax,dword ptr ds:[bx+(rc_list-rbase)]
        push    bx
        call    rm_classify
        pop     bx
        jc      rc_fail
        add     bx,4
        cmp     bx,16
        jb      rc_pass1

        ; And from that, how LRGDISK will.  LBA wherever it reaches the
        ; same sectors: when the BIOS uses it, and when the BIOS's CHS
        ; is the drive's own geometry - the BIOS's untranslated, or the
        ; drive's real 16 heads under a BIOS that translates by CHS -
        ; and the drive has LBA.  CHS, the BIOS's, only for a drive
        ; with no LBA.
        mov     byte ptr ds:[rc_step-rbase],10
        mov     al,byte ptr ds:[rc_how-rbase]
        mov     si,m_how_lba-rbase
        test    al,HOW_LBA
        jnz     rc_lba
        test    al,HOW_BIOS
        jz      rc_drvchs
        mov     cx,word ptr ds:[rc_heads-rbase]
        movzx   dx,byte ptr ds:[rc_spt-rbase]
        call    rm_ownchs
        mov     si,m_how_chs-rbase
        jne     rc_chs
        mov     si,m_how_same-rbase
        jmp     rc_lba
rc_drvchs:
        mov     cx,word ptr ds:[rc_cheads-rbase]
        mov     dx,word ptr ds:[rc_cspt-rbase]
        call    rm_ownchs
        jne     rc_fail
        mov     si,m_how_large-rbase
rc_lba:
        mov     byte ptr ds:[rc_mode-rbase],1
        jmp     rc_decided
rc_chs:
        mov     byte ptr ds:[rc_mode-rbase],0
rc_decided:
        mov     word ptr ds:[rc_howmsg-rbase],si

        ; 12-13: each again, through the BIOS and by LRGDISK that way
        xor     bx,bx
rc_pass2:
        mov     eax,dword ptr ds:[bx+(rc_list-rbase)]
        push    bx
        call    rm_compare
        pop     bx
        jc      rc_fail
        add     bx,4
        cmp     bx,16
        jb      rc_pass2

        ; 14-15: by LBA, the drive's last sector as well
        mov     word ptr ds:[rc_endmsg-rbase],m_crlf-rbase
        cmp     byte ptr ds:[rc_mode-rbase],0
        je      rc_good
        call    rm_endcheck
        jc      rc_fail
rc_good:

        ; the drive is LRGDISK's
        mov     si,word ptr ds:[rc_howmsg-rbase]
        call    rm_puts
        mov     si,word ptr ds:[rc_endmsg-rbase]
        call    rm_puts
        mov     si,word ptr ds:[rc_dmamsg-rbase]
        call    rm_puts
        mov     al,byte ptr ds:[rc_xfer-rbase]
        and     al,07h
        add     al,'0'
        cmp     word ptr ds:[rc_dmamsg-rbase],m_dma_udma-rbase
        je      rc_dmamode
        cmp     word ptr ds:[rc_dmamsg-rbase],m_dma_mw-rbase
        jne     rc_dmadone
rc_dmamode:
        call    rm_putc
rc_dmadone:
        mov     si,m_crlf-rbase
        call    rm_puts

        ; and how PIO will go: the block's size, and the data's width
        call    rm_blockmode
        mov     si,word ptr ds:[rc_blkmsg-rbase]
        call    rm_puts
        cmp     byte ptr ds:[rc_mult-rbase],0
        je      rc_blksaid
        movzx   ax,byte ptr ds:[rc_mult-rbase]
        call    rm_dec
        mov     si,word ptr ds:[rc_blkmsg2-rbase]
        call    rm_puts
rc_blksaid:
        mov     si,m_crlf-rbase
        call    rm_puts
        call    rm_try32
        mov     si,word ptr ds:[rc_32msg-rbase]
        call    rm_puts
        mov     si,m_crlf-rbase
        call    rm_puts

        call    rm_unitrec              ; DI -> its record
        mov     al,RF_OURS
        cmp     byte ptr ds:[rc_mode-rbase],0
        je      rc_flags
        or      al,RF_LBA
        cmp     byte ptr ds:[rc_48-rbase],0
        je      rc_flags
        or      al,RF_LBA48
rc_flags:
        cmp     word ptr ds:[rc_dmamsg-rbase],m_dma_udma-rbase
        je      rc_dmaflag
        cmp     word ptr ds:[rc_dmamsg-rbase],m_dma_mw-rbase
        jne     rc_noflag
rc_dmaflag:
        or      al,RF_DMA
rc_noflag:
        mov     ah,byte ptr ds:[rc_drive-rbase]
        sub     ah,80h
        shl     ah,RF_DISKSHIFT
        or      al,ah
        mov     byte ptr ds:[di+UR_FLAGS],al
        mov     al,byte ptr ds:[rc_drive-rbase]
        mov     byte ptr ds:[di+UR_DISK],al
        mov     ax,word ptr ds:[rc_heads-rbase]
        mov     word ptr ds:[di+UR_HEADS],ax
        dec     al
        mov     byte ptr ds:[di+UR_IHEADS],al
        mov     al,byte ptr ds:[rc_spt-rbase]
        mov     byte ptr ds:[di+UR_SPT],al
        mov     byte ptr ds:[di+UR_ISPT],al
        mov     al,byte ptr ds:[rc_xfer-rbase]
        mov     byte ptr ds:[di+UR_XFER],al
        mov     ax,word ptr ds:[rc_maxcyl-rbase]
        add     ax,2
        mov     word ptr ds:[di+UR_CYLS],ax
        movzx   eax,word ptr ds:[rc_tcyl-rbase] ; the last sector: the
        movzx   ecx,word ptr ds:[rc_heads-rbase] ; table's by CHS, the
        imul    eax,ecx                 ; drive's own by LBA
        movzx   ecx,byte ptr ds:[rc_spt-rbase]
        imul    eax,ecx
        cmp     byte ptr ds:[rc_mode-rbase],0
        je      rc_last
        cmp     dword ptr ds:[rc_cap-rbase],0
        je      rc_last
        mov     eax,dword ptr ds:[rc_cap-rbase]
rc_last:
        dec     eax
        mov     dword ptr ds:[di+UR_MAXSEC],eax
        movzx   bx,byte ptr ds:[rc_cable-rbase]
        or      byte ptr ds:[bx+(rv_cbl-rbase)],1
        pop     bx
        ret

rc_failes:
        push    cs
        pop     es
rc_fail:
        mov     si,m_check-rbase
        call    rm_puts
        movzx   ax,byte ptr ds:[rc_step-rbase]
        call    rm_dec
        mov     si,m_value-rbase
        call    rm_puts
        mov     eax,dword ptr ds:[rc_val-rbase]
        mov     cl,8
        call    rm_hex
        mov     si,m_close-rbase
        call    rm_puts
        movzx   bx,byte ptr ds:[rc_step-rbase]
        add     bx,bx
        mov     si,word ptr ds:[bx+(rv_whys-2-rbase)]
        call    rm_puts
        mov     si,m_crlf-rbase
        call    rm_puts
        pop     bx
        ret

;---------------------------------------------------------------------
; rm_setcable - AL = 0 or 1: the cable the port routines work
;---------------------------------------------------------------------
rm_setcable:
        mov     byte ptr ds:[rc_cable-rbase],al
        mov     word ptr ds:[rc_base-rbase],IDE0_DATA
        mov     word ptr ds:[rc_ctl-rbase],IDE0_CTRL
        or      al,al
        jz      rsc_out
        mov     word ptr ds:[rc_base-rbase],IDE1_DATA
        mov     word ptr ds:[rc_ctl-rbase],IDE1_CTRL
rsc_out:
        ret

;---------------------------------------------------------------------
; rm_unitrec - DI -> the unit record of rc_cable, rc_unit
;---------------------------------------------------------------------
rm_unitrec:
        mov     al,byte ptr ds:[rc_cable-rbase]
        shl     al,1
        add     al,byte ptr ds:[rc_unit-rbase]
        mov     ah,UR_SIZE
        mul     ah
        mov     di,ax
        add     di,rv_units-rbase
        ret

;---------------------------------------------------------------------
; rm_cablename - " of the first cable" or " of the second cable"
;---------------------------------------------------------------------
rm_cablename:
        mov     si,m_cable1-rbase
        cmp     byte ptr ds:[rc_cable-rbase],0
        je      rcn_say
        mov     si,m_cable2-rbase
rcn_say:
        call    rm_puts
        ret

;---------------------------------------------------------------------
; rm_topbit - AL = a byte with bits set -> AL = the number of its
; highest set bit
;---------------------------------------------------------------------
rm_topbit:
        push    cx
        xor     cl,cl
rtb_next:
        shr     al,1
        jz      rtb_done
        inc     cl
        jmp     rtb_next
rtb_done:
        mov     al,cl
        pop     cx
        ret

;---------------------------------------------------------------------
; rm_findbm - the PCI bus-master IDE engine, through the PCI BIOS: the
; first controller of class 01h 01h, its BAR4 (the engine's sixteen
; ports) and its programming interface (bits 0 and 2: a cable in native
; mode is not at 1F0h or 170h, so its eight are not for that cable).
; The controller is told it may master the bus.  rv_bm has each
; cable's engine port, or 0.  rv_pciide says there is a PCI controller
; at all, engine or no engine and LRGDiskDMA or not: a host that may
; be tried a doubleword at a time (rm_try32).
;---------------------------------------------------------------------
rm_findbm:
        mov     dword ptr ds:[rv_bm-rbase],0
        mov     byte ptr ds:[rv_pciide-rbase],0
        mov     ax,0B101h               ; PCI BIOS present?
        int     1Ah
        jc      rfb_nopci
        or      ah,ah
        jnz     rfb_nopci
        cmp     edx,20494350h           ; 'PCI '
        jne     rfb_nopci
        mov     byte ptr ds:[rv_lastbus-rbase],cl
        xor     bh,bh                   ; the bus
rfb_bus:
        xor     bl,bl                   ; the device and function
rfb_dev:
        mov     ax,0B10Ah               ; the class dword
        mov     di,08h
        int     1Ah
        jc      rfb_next
        shr     ecx,16
        cmp     cx,0101h
        je      rfb_found
rfb_next:
        inc     bl
        jnz     rfb_dev
        inc     bh
        cmp     bh,byte ptr ds:[rv_lastbus-rbase]
        jbe     rfb_bus
rfb_nopci:
        cmp     byte ptr ds:[rv_dma-rbase],0
        jne     rfb_none
rfb_off:
        mov     si,m_bm_off-rbase
        jmp     rm_puts
rfb_none:
        mov     si,m_bm_none-rbase
        jmp     rm_puts

rfb_found:
        mov     byte ptr ds:[rv_pciide-rbase],1
        cmp     byte ptr ds:[rv_dma-rbase],0
        je      rfb_off
        mov     ax,0B10Ah               ; BAR4: an I/O range
        mov     di,20h
        int     1Ah
        jc      rfb_none
        test    cl,1
        jz      rfb_none
        and     cx,0FFFCh
        jz      rfb_none
        mov     dx,cx
        push    dx
        mov     ax,0B10Ah               ; the programming interface
        mov     di,08h
        int     1Ah
        pop     dx
        test    ch,1
        jnz     rfb_second
        mov     word ptr ds:[rv_bm-rbase],dx
rfb_second:
        test    ch,4
        jnz     rfb_cmd
        mov     ax,dx
        add     ax,8
        mov     word ptr ds:[rv_bm+2-rbase],ax
rfb_cmd:
        push    dx
        mov     ax,0B109h               ; the command register: I/O and
        mov     di,04h                  ; bus mastering on
        int     1Ah
        or      cx,0005h
        mov     ax,0B10Ch
        mov     di,04h
        int     1Ah
        pop     dx
        mov     si,m_bm-rbase
        call    rm_puts
        movzx   eax,dx
        mov     cl,4
        call    rm_hex
        mov     si,m_bm2-rbase
        call    rm_puts
        movzx   ax,bh
        call    rm_dec
        mov     si,m_bm2b-rbase
        call    rm_puts
        movzx   ax,bl
        shr     ax,3
        call    rm_dec
        mov     si,m_bm3-rbase
        call    rm_puts
        cmp     word ptr ds:[rv_bm-rbase],0
        jne     rfb_say2
        mov     si,m_bm_nat1-rbase
        call    rm_puts
rfb_say2:
        cmp     word ptr ds:[rv_bm+2-rbase],0
        jne     rfb_said
        mov     si,m_bm_nat2-rbase
        call    rm_puts
rfb_said:
        mov     si,m_crlf-rbase
        call    rm_puts
        ret

;---------------------------------------------------------------------
; rm_chipbugs - two PCI IDE controllers with data corruption bugs,
; found through the PCI BIOS and set as Linux sets them: the PC-Tech
; RZ1000 (1042h:1000h, and the RZ1001) corrupts a PIO read when its
; read-ahead is on and the processor is interrupted between sectors -
; read-ahead off (register 40h bit 13); the CMD640 (1095h:0640h)
; corrupts data when both its cables transfer at once, and with
; read-ahead on - read-ahead off for all four drives (CNTRL 51h bits 6
; and 7, ARTTIM23 57h bits 2 and 3) and the cables used one at a time
; (rv_serial), and its data port never read a doubleword at a time
; (rv_no32: Linux, the same).  Nothing is written to a controller that
; is neither.
;---------------------------------------------------------------------
rm_chipbugs:
        mov     dx,1042h                ; the RZ1000, or the RZ1001
        mov     cx,1000h
        call    rm_pcifind
        jnc     rcb_rz
        mov     cx,1001h
        call    rm_pcifind
        jc      rcb_cmd
rcb_rz:
        mov     ax,0B109h
        mov     di,40h
        int     1Ah
        jc      rcb_cmd
        and     cx,0DFFFh
        mov     ax,0B10Ch
        mov     di,40h
        int     1Ah
        mov     si,m_rz1000-rbase
        call    rm_puts
rcb_cmd:
        mov     dx,1095h                ; the CMD640
        mov     cx,0640h
        call    rm_pcifind
        jc      rcb_out
        mov     ax,0B108h
        mov     di,51h
        int     1Ah
        jc      rcb_out
        or      cl,0C0h
        mov     ax,0B10Bh
        mov     di,51h
        int     1Ah
        mov     ax,0B108h
        mov     di,57h
        int     1Ah
        jc      rcb_out
        or      cl,0Ch
        mov     ax,0B10Bh
        mov     di,57h
        int     1Ah
        mov     byte ptr ds:[rv_serial-rbase],1
        mov     byte ptr ds:[rv_no32-rbase],1 ; (its 32-bit path IS the
        mov     si,m_cmd640-rbase       ; read-ahead)
        call    rm_puts
rcb_out:
        cmp     byte ptr ds:[rv_serial-rbase],0
        je      rcb_said
        mov     si,m_serial-rbase
        call    rm_puts
rcb_said:
        ret

; rm_pcifind - DX = vendor, CX = device: the first such PCI device,
; BH = its bus, BL = its device and function.  CF if none, or no PCI
; BIOS.
rm_pcifind:
        push    si
        xor     si,si
        mov     ax,0B102h
        int     1Ah
        pop     si
        jc      rpf_out
        or      ah,ah
        jz      rpf_out
        stc
rpf_out:
        ret

;---------------------------------------------------------------------
; rm_ownchs - CX heads, DX sectors: is that the drive's own geometry
; (IDENTIFY's words 3 and 6), with LBA there to reach it by?  ZF if
; so.
;---------------------------------------------------------------------
rm_ownchs:
        cmp     byte ptr ds:[rc_lbaok-rbase],1
        jne     roc_out
        cmp     cx,word ptr ds:[rc_dheads-rbase]
        jne     roc_out
        cmp     dx,word ptr ds:[rc_dspt-rbase]
roc_out:
        ret

;---------------------------------------------------------------------
; rm_clamp - EAX = a sector in the BIOS's numbering, cut to its last
; -> rc_t, and rc_val for a failure to report
;---------------------------------------------------------------------
rm_clamp:
        cmp     eax,dword ptr ds:[rc_total-rbase]
        jb      rcl_in
        mov     eax,dword ptr ds:[rc_total-rbase]
        dec     eax
rcl_in:
        mov     dword ptr ds:[rc_t-rbase],eax
        mov     dword ptr ds:[rc_val-rbase],eax
        ret

;---------------------------------------------------------------------
; rm_classify - EAX = a sector: read it through the BIOS, the registers
; poisoned first - a sector number of 0 and a cylinder of FFFFh - so
; that a read made through any other cable or drive leaves them so,
; and narrow rc_how to what the registers it left are consistent with:
; an LBA equal to the sector number (HOW_LBA), the CHS of the BIOS's
; geometry (HOW_BIOS), the CHS of the geometry the drive is set to
; (HOW_DRIVE).  rc_step and rc_val say what failed; CF.
;---------------------------------------------------------------------
rm_classify:
        call    rm_clamp

        ; 8: the BIOS's read, once more if it fails
        mov     byte ptr ds:[rc_step-rbase],8
        call    rm_poison
        call    rm_biosread
        jnc     rk_read
        call    rm_poison
        call    rm_biosread
        jnc     rk_read
        mov     byte ptr ds:[rc_val-rbase],ah
        stc
        ret
rk_read:
        ; 9: through this cable, at this drive
        mov     byte ptr ds:[rc_step-rbase],9
        call    rm_regs
        shr     eax,28                  ; drive/head's bit 4
        and     al,1
        cmp     al,byte ptr ds:[rc_unit-rbase]
        jne     rk_fail
        cmp     byte ptr ds:[rc_tf-rbase],0
        jne     rk_moved
        cmp     word ptr ds:[rc_tf+1-rbase],0FFFFh
        je      rk_fail
rk_moved:
        ; 10, 11: how.  A drive leaves the address of the sector it read
        ; - or, as some do (and QEMU's), of the one after it; either is
        ; taken, as the same numbering.
        mov     byte ptr ds:[rc_step-rbase],10
        xor     bl,bl
        test    byte ptr ds:[rc_tf+3-rbase],40h
        jz      rk_chs
        mov     eax,dword ptr ds:[rc_tf-rbase]
        and     eax,0FFFFFFFh
        sub     eax,dword ptr ds:[rc_t-rbase]
        cmp     eax,1
        ja      rk_narrow
        mov     byte ptr ds:[rc_step-rbase],11
        cmp     byte ptr ds:[rc_lbaok-rbase],0
        je      rk_fail
        mov     byte ptr ds:[rc_step-rbase],10
        mov     bl,HOW_LBA
        jmp     rk_narrow
rk_chs:
        movzx   ecx,word ptr ds:[rc_heads-rbase]
        movzx   edx,byte ptr ds:[rc_spt-rbase]
        call    rm_chsnear
        jne     rk_drive
        or      bl,HOW_BIOS
rk_drive:
        movzx   ecx,word ptr ds:[rc_cheads-rbase]
        movzx   edx,word ptr ds:[rc_cspt-rbase]
        call    rm_chsnear
        jne     rk_narrow
        or      bl,HOW_DRIVE
rk_narrow:
        and     byte ptr ds:[rc_how-rbase],bl
        jz      rk_fail
        clc
        ret
rk_fail:
        stc
        ret

;---------------------------------------------------------------------
; rm_chsnear - is rc_tf the CHS of sector rc_t, or of the one after it,
; in a geometry of ECX heads and EDX sectors?  ZF if so.  EAX changed.
; rm_chsis - the same for sector EAX alone; EAX, ECX, EDX changed.
;---------------------------------------------------------------------
rm_chsnear:
        push    ecx
        push    edx
        mov     eax,dword ptr ds:[rc_t-rbase]
        call    rm_chsis
        pop     edx
        pop     ecx
        je      rcn_out
        push    ecx
        push    edx
        mov     eax,dword ptr ds:[rc_t-rbase]
        inc     eax
        call    rm_chsis
        pop     edx
        pop     ecx
rcn_out:
        ret

rm_chsis:
        or      ecx,ecx
        jz      rci_no
        or      edx,edx
        jz      rci_no
        push    ecx
        mov     ecx,edx
        xor     edx,edx
        div     ecx                     ; EAX = track, EDX = sector - 1
        pop     ecx
        inc     dl
        cmp     dl,byte ptr ds:[rc_tf-rbase]
        jne     rci_out
        xor     edx,edx
        div     ecx                     ; EAX = cylinder, EDX = head
        cmp     eax,0FFFFh
        ja      rci_no
        cmp     ax,word ptr ds:[rc_tf+1-rbase]
        jne     rci_out
        mov     al,byte ptr ds:[rc_tf+3-rbase]
        and     al,0Fh
        cmp     al,dl                   ; (a head past 15 never matches)
rci_out:
        ret
rci_no:
        test    sp,sp                   ; NZ
        ret

;---------------------------------------------------------------------
; rm_regs - the cable's +3 to +6 into rc_tf, and into rc_val and EAX
;---------------------------------------------------------------------
rm_regs:
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_SECTOR
        in      al,dx
        mov     byte ptr ds:[rc_tf-rbase],al
        inc     dx
        in      al,dx
        mov     byte ptr ds:[rc_tf+1-rbase],al
        inc     dx
        in      al,dx
        mov     byte ptr ds:[rc_tf+2-rbase],al
        inc     dx
        in      al,dx
        mov     byte ptr ds:[rc_tf+3-rbase],al
        mov     eax,dword ptr ds:[rc_tf-rbase]
        mov     dword ptr ds:[rc_val-rbase],eax
        ret

;---------------------------------------------------------------------
; rm_whichcable - after the BIOS's read of sector 0 with both cables'
; registers poisoned: the cable whose selected drive is ready and
; shows cylinder 0 is the one the read went to; rc_cable, rc_base,
; rc_ctl and rc_unit set to it.  CF if neither does, rc_val = the last
; cable's status and registers.
;---------------------------------------------------------------------
rm_whichcable:
        xor     al,al
rwc_try:
        push    ax
        call    rm_setcable
        call    rm_regs                 ; rc_tf, rc_val = +3 to +6
        mov     dx,word ptr ds:[rc_ctl-rbase]
        in      al,dx                   ; the status over the drive/head
        mov     byte ptr ds:[rc_val+3-rbase],al ; byte, for the report
        cmp     al,0FFh                 ; nothing there
        je      rwc_next
        cmp     al,7Fh
        je      rwc_next
        test    al,ST_BSY
        jnz     rwc_next
        test    al,ST_DRDY
        jz      rwc_next
        cmp     word ptr ds:[rc_tf+1-rbase],0 ; cylinder 0: the read
        jne     rwc_next
        pop     ax
        mov     al,byte ptr ds:[rc_tf+3-rbase]
        shr     al,4
        and     al,1
        mov     byte ptr ds:[rc_unit-rbase],al
        clc
        ret
rwc_next:
        pop     ax
        inc     al
        cmp     al,2
        jb      rwc_try
        stc
        ret

;---------------------------------------------------------------------
; rm_compare - EAX = a sector: through the BIOS into rv_buf1, by
; LRGDISK's way (rc_mode) into rv_buf2, and the same bytes.  CF.
;---------------------------------------------------------------------
rm_compare:
        call    rm_clamp
        mov     byte ptr ds:[rc_step-rbase],8
        call    rm_biosread
        jnc     rcp_read
        call    rm_biosread
        jnc     rcp_read
        mov     byte ptr ds:[rc_val-rbase],ah
        stc
        ret
rcp_read:
        mov     byte ptr ds:[rc_step-rbase],12
        call    rm_ownread
        jc      rcp_fail
        mov     byte ptr ds:[rc_step-rbase],13
        call    rm_same
        jne     rcp_fail
        clc
        ret
rcp_fail:
        stc
        ret

rm_same:                                ; rv_buf1 against rv_buf2: ZF
        mov     si,rv_buf1-rbase
        mov     di,rv_buf2-rbase
        mov     cx,256
        repe    cmpsw
        ret

;---------------------------------------------------------------------
; rm_endcheck - by LBA, the drive's last sector: read through the
; BIOS's INT 13h extensions, if it has them, and by LRGDISK, and
; compared.  The registers the BIOS leaves must name the sector (or the
; one after it, as rm_classify allows) in their low 24 bits: p3bf's
; Award BIOS reads with a 48-bit command and the LBA bit clear (A0h in
; the drive register, the sector's LBA in the three below), so the
; drive register is not looked at.  Past 28 bits, where only a 48-bit
; command reaches, the high-order bytes must name it too (rm_hob).
;
; A BIOS that cannot read that far - or reads somewhere else, a 28-bit
; command with the top bits lost - has the last sector 28 bits reach
; checked instead: LRGDISK goes on past where the BIOS stops, as it
; goes past the BIOS's last cylinder.  rc_endmsg says what was checked.
; CF if it failed.
;---------------------------------------------------------------------
rm_endcheck:
        mov     word ptr ds:[rc_endmsg-rbase],m_end_noext-rbase
        mov     eax,dword ptr ds:[rc_cap-rbase]
        or      eax,eax
        jz      rec_skip
        dec     eax
        mov     dword ptr ds:[rc_t-rbase],eax
        mov     dword ptr ds:[rc_val-rbase],eax

        mov     ah,41h
        mov     bx,55AAh
        mov     dl,byte ptr ds:[rc_drive-rbase]
        call    rm_rom13
        jc      rec_skip
        cmp     bx,0AA55h
        jne     rec_skip
        test    cl,1
        jz      rec_skip

        mov     word ptr ds:[rc_endmsg-rbase],m_end_ok-rbase
        call    rm_bios42
        jnc     rec_own
        cmp     dword ptr ds:[rc_t-rbase],LBA28_END
        jb      rec_fail
        mov     eax,LBA28_END-1
        mov     dword ptr ds:[rc_t-rbase],eax
        mov     dword ptr ds:[rc_val-rbase],eax
        mov     word ptr ds:[rc_endmsg-rbase],m_end_28-rbase
        call    rm_bios42
        jc      rec_fail
rec_own:
        ; 12-13: by LRGDISK, and the same bytes
        mov     byte ptr ds:[rc_step-rbase],12
        call    rm_ownread
        jc      rec_fail
        mov     byte ptr ds:[rc_step-rbase],13
        call    rm_same
        jne     rec_fail
rec_skip:
        clc
        ret
rec_fail:
        stc
        ret

;---------------------------------------------------------------------
; rm_bios42 - 14: sector rc_t through the BIOS's AH=42h into rv_buf1,
; the registers poisoned first; 15: the registers it left name it, at
; this unit.  CF if not, rc_val saying what was seen.
;---------------------------------------------------------------------
rm_bios42:
        mov     byte ptr ds:[rc_step-rbase],14
        call    rm_poison
        mov     word ptr ds:[rv_dap-rbase],0010h
        mov     word ptr ds:[rv_dap+2-rbase],1
        mov     word ptr ds:[rv_dap+4-rbase],rv_buf1-rbase
        mov     word ptr ds:[rv_dap+6-rbase],cs
        mov     eax,dword ptr ds:[rc_t-rbase]
        mov     dword ptr ds:[rv_dap+8-rbase],eax
        mov     dword ptr ds:[rv_dap+12-rbase],0
        mov     ah,42h
        mov     dl,byte ptr ds:[rc_drive-rbase]
        mov     si,rv_dap-rbase
        call    rm_rom13
        jnc     rbx_read
        mov     byte ptr ds:[rc_val-rbase],ah
        stc
        ret
rbx_read:
        mov     byte ptr ds:[rc_step-rbase],15
        call    rm_regs
        shr     eax,28
        and     al,1
        cmp     al,byte ptr ds:[rc_unit-rbase]
        jne     rbx_fail
        mov     eax,dword ptr ds:[rc_tf-rbase]
        sub     eax,dword ptr ds:[rc_t-rbase]
        and     eax,0FFFFFFh            ; the sector or the one after
        cmp     eax,1
        ja      rbx_fail
        add     eax,dword ptr ds:[rc_t-rbase]
        cmp     dword ptr ds:[rc_t-rbase],LBA28_END
        jb      rbx_ok
        push    eax
        call    rm_hob
        pop     edx
        shr     edx,24
        cmp     eax,edx
        jne     rbx_fail
rbx_ok:
        clc
        ret
rbx_fail:
        stc
        ret

;---------------------------------------------------------------------
; rm_hob - the high-order bytes of +3 to +5 (the device control's HOB
; bit) as EAX bits 0-23, and into rc_val
;---------------------------------------------------------------------
rm_hob:
        call    rm_quiet
        mov     dx,word ptr ds:[rc_ctl-rbase]
        mov     al,CTL_HOB OR CTL_NIEN OR 08h
        out     dx,al
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_SECTOR
        in      al,dx
        mov     byte ptr ds:[rc_val-rbase],al
        inc     dx
        in      al,dx
        mov     byte ptr ds:[rc_val+1-rbase],al
        inc     dx
        in      al,dx
        mov     byte ptr ds:[rc_val+2-rbase],al
        mov     byte ptr ds:[rc_val+3-rbase],0
        call    rm_loud
        mov     eax,dword ptr ds:[rc_val-rbase]
        ret

;---------------------------------------------------------------------
; rm_poison - select rc_unit of the cable and put sector 0, cylinder
; FFFFh in the registers - and FFh in each one's high-order byte,
; which a 28-bit command's write pushes out for the value below it.
; rm_poison2 - both units of both cables, each cable's master left
; selected.
;---------------------------------------------------------------------
rm_poison:
        mov     al,byte ptr ds:[rc_unit-rbase]
rm_poisonu:                             ; AL = the unit
        shl     al,4
        or      al,0A0h
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_DRVHD
        out     dx,al
        call    rm_delay
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_SECTOR
        mov     al,0FFh
        out     dx,al
        xor     al,al
        out     dx,al
        inc     dx
        mov     al,0FFh
        out     dx,al
        out     dx,al
        inc     dx
        out     dx,al
        out     dx,al
        ret

rm_poison2:
        push    ax
        xor     al,al
rp2_cable:
        push    ax
        call    rm_setcable
        mov     al,1
        call    rm_poisonu
        xor     al,al
        call    rm_poisonu
        pop     ax
        inc     al
        cmp     al,2
        jb      rp2_cable
        pop     ax
        ret

;---------------------------------------------------------------------
; rm_biosread - sector rc_t of rc_drive into rv_buf1 by the ROM's
; INT 13h, in the BIOS's CHS.  CF, AH = the BIOS's error.
;---------------------------------------------------------------------
rm_biosread:
        mov     eax,dword ptr ds:[rc_t-rbase]
        xor     edx,edx
        movzx   ecx,byte ptr ds:[rc_spt-rbase]
        div     ecx
        inc     dl
        mov     byte ptr ds:[rc_s-rbase],dl
        xor     edx,edx
        movzx   ecx,word ptr ds:[rc_heads-rbase]
        div     ecx                     ; EAX = cylinder, DL = head
        mov     dh,dl
        mov     ch,al
        mov     cl,ah
        shl     cl,6
        or      cl,byte ptr ds:[rc_s-rbase]
        mov     dl,byte ptr ds:[rc_drive-rbase]
        mov     bx,rv_buf1-rbase
        mov     ax,0201h
        call    rm_rom13
        ret

;---------------------------------------------------------------------
; rm_rom13 - the ROM's INT 13h, as INT 13h would call it.  DS and ES
; kept.
;---------------------------------------------------------------------
rm_rom13:
        push    ds
        push    es
        pushf
        cli
        call    dword ptr cs:[rv_rom13-rbase]
        sti
        pop     es
        pop     ds
        ret

;---------------------------------------------------------------------
; rm_ownread - sector rc_t of rc_unit into rv_buf2 through the ports,
; addressed by rc_mode.  CF, rc_val's low byte = the status.
;---------------------------------------------------------------------
rm_ownread:
        call    rm_quiet
        mov     eax,dword ptr ds:[rc_t-rbase]
        mov     bl,byte ptr ds:[rc_unit-rbase]
        shl     bl,4
        mov     byte ptr ds:[rc_ext-rbase],0
        cmp     byte ptr ds:[rc_mode-rbase],0
        je      ro_chs
        mov     dword ptr ds:[rc_tf-rbase],eax
        and     byte ptr ds:[rc_tf+3-rbase],0Fh
        cmp     eax,LBA28_END           ; past 28 bits: READ SECTORS EXT
        jb      ro_lba
        mov     byte ptr ds:[rc_ext-rbase],1
        mov     byte ptr ds:[rc_tf+3-rbase],0
ro_lba:
        or      bl,0E0h
        or      byte ptr ds:[rc_tf+3-rbase],bl
        jmp     ro_go
ro_chs:
        xor     edx,edx
        movzx   ecx,byte ptr ds:[rc_spt-rbase]
        div     ecx
        inc     dl
        mov     byte ptr ds:[rc_tf-rbase],dl
        xor     edx,edx
        movzx   ecx,word ptr ds:[rc_heads-rbase]
        div     ecx
        mov     word ptr ds:[rc_tf+1-rbase],ax
        or      bl,dl
        or      bl,0A0h
        mov     byte ptr ds:[rc_tf+3-rbase],bl
ro_go:
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_DRVHD
        mov     al,byte ptr ds:[rc_tf+3-rbase]
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      ro_fail
        mov     dx,word ptr ds:[rc_base-rbase]
        inc     dx                      ; features
        cmp     byte ptr ds:[rc_ext-rbase],0
        je      ro_low
        xor     al,al                   ; the high-order bytes first
        out     dx,al
        inc     dx
        out     dx,al
        inc     dx
        mov     al,byte ptr ds:[rc_t+3-rbase]
        out     dx,al
        inc     dx
        xor     al,al
        out     dx,al
        inc     dx
        out     dx,al
        sub     dx,R_CYLHI-R_FEAT
ro_low:
        xor     al,al
        out     dx,al
        inc     dx
        mov     al,1
        out     dx,al
        inc     dx
        mov     al,byte ptr ds:[rc_tf-rbase]
        out     dx,al
        inc     dx
        mov     al,byte ptr ds:[rc_tf+1-rbase]
        out     dx,al
        inc     dx
        mov     al,byte ptr ds:[rc_tf+2-rbase]
        out     dx,al
        add     dx,R_CMD-R_CYLHI
        mov     al,ATA_READ
        cmp     byte ptr ds:[rc_ext-rbase],0
        je      ro_cmd
        mov     al,ATA_READ_EXT
ro_cmd:
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      ro_fail
        test    al,ST_ERR
        jnz     ro_fail
        test    al,ST_DRQ
        jz      ro_fail
        mov     di,rv_buf2-rbase
        mov     cx,256
        mov     dx,word ptr ds:[rc_base-rbase]
        rep     insw
        call    rm_loud
        clc
        ret
ro_fail:
        mov     byte ptr ds:[rc_val-rbase],al
        call    rm_loud
        stc
        ret

;---------------------------------------------------------------------
; rm_identify - IDENTIFY DEVICE to unit AL of the cable, into rv_buf2.
; CF if it does not answer as an ATA disk does; rc_val's low byte =
; the status.
;---------------------------------------------------------------------
rm_identify:
        call    rm_quiet
        shl     al,4
        or      al,0A0h
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_DRVHD
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      ri_fail
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_CMD
        mov     al,ATA_IDENTIFY
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      ri_fail
        test    al,ST_ERR
        jnz     ri_fail
        test    al,ST_DRQ
        jz      ri_fail
        mov     di,rv_buf2-rbase
        mov     cx,256
        mov     dx,word ptr ds:[rc_base-rbase]
        rep     insw
        call    rm_loud
        clc
        ret
ri_fail:
        mov     byte ptr ds:[rc_val-rbase],al
        call    rm_loud
        stc
        ret

;---------------------------------------------------------------------
; rm_blockmode - the drive is LRGDISK's: how many sectors a block it
; will move by PIO (READ and WRITE MULTIPLE).  The size the drive is
; set to is kept - the BIOS set it, and reads with it - and a drive
; set to blocks of one sector is left so.  A drive set to none is set
; here: the highest power of two it takes, LD_BLOCKMAX at most; the
; drive's taking the command is what says it is set (QEMU's IDENTIFY
; goes on saying what it said the first time it was asked).  rc_mult =
; the size, 0 for a sector at a time; rc_blkmsg and rc_blkmsg2 say
; which, around the number.  The protected-mode side works the size
; out the same way and gives the drive the command again (ld_initbdd).
;---------------------------------------------------------------------
rm_blockmode:
        mov     byte ptr ds:[rc_mult-rbase],0
        mov     word ptr ds:[rc_blkmsg-rbase],m_blk_off-rbase
        cmp     byte ptr ds:[rv_block-rbase],0
        je      rbm_out
        mov     word ptr ds:[rc_blkmsg2-rbase],m_blk_was-rbase
        mov     al,byte ptr ds:[rc_mcur-rbase]
IFDEF LD_TESTSET                        ; (a rig whose BIOS always sets a
        xor     al,al                   ; size: taken to have set none, so
ENDIF                                   ; that the setting below is run)
        mov     word ptr ds:[rc_blkmsg-rbase],m_blk_one-rbase
        cmp     al,1
        je      rbm_out
        ja      rbm_have
        mov     word ptr ds:[rc_blkmsg-rbase],m_blk_none-rbase
        mov     al,byte ptr ds:[rc_mmax-rbase]
        cmp     al,2
        jb      rbm_out
        mov     ah,LD_BLOCKMAX
rbm_fit:
        cmp     ah,al
        jbe     rbm_set
        shr     ah,1
        jmp     rbm_fit
rbm_set:
        mov     word ptr ds:[rc_blkmsg-rbase],m_blk_refused-rbase
        mov     al,ah
        push    ax
        call    rm_setmult
        pop     ax                      ; (AL = the size again)
        jc      rbm_out
        mov     word ptr ds:[rc_blkmsg2-rbase],m_blk_set-rbase
rbm_have:
        mov     byte ptr ds:[rc_mult-rbase],al
        mov     word ptr ds:[rc_blkmsg-rbase],m_blk_in-rbase
rbm_out:
        ret

;---------------------------------------------------------------------
; rm_setmult - SET MULTIPLE MODE to rc_unit: AL sectors a block.  CF
; if the drive refuses.
;---------------------------------------------------------------------
rm_setmult:
        push    ax
        call    rm_quiet
        mov     al,byte ptr ds:[rc_unit-rbase]
        shl     al,4
        or      al,0A0h
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_DRVHD
        out     dx,al
        call    rm_delay
        call    rm_bsy
        pop     ax
        jc      rsm_fail
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_COUNT
        out     dx,al
        add     dx,R_CMD-R_COUNT
        mov     al,ATA_SETMULT
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      rsm_fail
        test    al,ST_ERR
        jnz     rsm_fail
        call    rm_loud
        clc
        ret
rsm_fail:
        call    rm_loud
        stc
        ret

;---------------------------------------------------------------------
; rm_try32 - the drive is LRGDISK's: will its data go a doubleword
; at a time?  Only where that is offered - by the drive (rc_w48) or by
; a PCI controller - and never on a CMD640; and then only if the
; IDENTIFY block comes the same both ways (rm_ident32).  rc_32msg
; says, and why not.  This is the report's account: the
; protected-mode side makes the measurement again for itself
; (ld_prove32), having no way to be told.
;---------------------------------------------------------------------
rm_try32:
        mov     word ptr ds:[rc_32msg-rbase],m_32_off-rbase
        cmp     byte ptr ds:[rv_io32-rbase],0
        je      r32_out
        mov     word ptr ds:[rc_32msg-rbase],m_32_cmd-rbase
        cmp     byte ptr ds:[rv_no32-rbase],0
        jne     r32_out
        mov     word ptr ds:[rc_32msg-rbase],m_32_none-rbase
        cmp     byte ptr ds:[rc_w48-rbase],0
        jne     r32_try
        cmp     byte ptr ds:[rv_pciide-rbase],0
        je      r32_out
r32_try:
        mov     word ptr ds:[rc_32msg-rbase],m_32_bad-rbase
        mov     al,byte ptr ds:[rc_unit-rbase]
        call    rm_identify             ; rv_buf2: a word at a time
        jc      r32_out
        call    rm_ident32              ; rv_buf1: a doubleword at a time
        jc      r32_out
        call    rm_same
        jne     r32_out
        mov     word ptr ds:[rc_32msg-rbase],m_32_on-rbase
r32_out:
        ret

;---------------------------------------------------------------------
; rm_ident32 - IDENTIFY DEVICE to rc_unit, into rv_buf1, taken a
; doubleword at a time.  CF if the drive does not answer, or has data
; left when 128 of them have been read - a host that cannot latch two
; words took half - which is read and dropped.
;---------------------------------------------------------------------
rm_ident32:
        call    rm_quiet
        mov     al,byte ptr ds:[rc_unit-rbase]
        shl     al,4
        or      al,0A0h
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_DRVHD
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      r3i_fail
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_CMD
        mov     al,ATA_IDENTIFY
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      r3i_fail
        test    al,ST_ERR
        jnz     r3i_fail
        test    al,ST_DRQ
        jz      r3i_fail
        mov     di,rv_buf1-rbase
        mov     cx,128
        mov     dx,word ptr ds:[rc_base-rbase]
        rep     insd
        call    rm_delay
        xor     bx,bx                   ; BX = words it still had
        mov     cx,256                  ; (no block has more)
r3i_drain:
        mov     dx,word ptr ds:[rc_ctl-rbase]
        in      al,dx
        test    al,ST_BSY
        jnz     r3i_next
        test    al,ST_DRQ
        jz      r3i_done
        mov     dx,word ptr ds:[rc_base-rbase]
        in      ax,dx
        inc     bx
r3i_next:
        loop    r3i_drain
r3i_done:
        call    rm_loud
        or      bx,bx
        jnz     r3i_no
        clc
        ret
r3i_fail:
        call    rm_loud
r3i_no:
        stc
        ret

;---------------------------------------------------------------------
; rm_present - is there anything at unit AL of the cable?  CF clear
; if so.  A status of FFh or 7Fh is nothing there.  Anything else is
; given IDENTIFY DEVICE, which an ATA drive answers and an ATAPI drive
; refuses with its signature, 14h EBh, in the cylinder registers -
; zeroed first, so that a cable with nothing there cannot show it.  A
; drive that stays busy counts as there.
;---------------------------------------------------------------------
rm_present:
        mov     dword ptr ds:[rv_pst-rbase],0
        push    ax
        call    rm_quiet
        pop     ax
        shl     al,4
        or      al,0A0h
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_DRVHD
        out     dx,al
        call    rm_delay
        xor     al,al
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_CYLLO
        out     dx,al
        inc     dx
        out     dx,al
        add     dx,R_STATUS-R_CYLHI
        in      al,dx
        mov     byte ptr ds:[rv_pst-rbase],al
        cmp     al,0FFh
        je      rp_none
        cmp     al,7Fh
        je      rp_none
rp_ask:
        call    rm_bsy
        jc      rp_some
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_CMD
        mov     al,ATA_IDENTIFY
        out     dx,al
        call    rm_delay
        call    rm_bsy
        mov     byte ptr ds:[rv_pst+1-rbase],al
        jc      rp_some
        test    al,ST_DRQ
        jnz     rp_drain
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_CYLLO
        in      al,dx
        mov     ah,al
        inc     dx
        in      al,dx
        mov     word ptr ds:[rv_pst+2-rbase],ax
        cmp     ax,14EBh
        je      rp_some
        cmp     ax,6996h                ; ...and a SATA one's
        je      rp_some
rp_none:
        call    rm_loud
        stc
        ret
rp_drain:
        mov     di,rv_buf2-rbase
        mov     cx,256
        mov     dx,word ptr ds:[rc_base-rbase]
        rep     insw
rp_some:
        call    rm_loud
        clc
        ret

;---------------------------------------------------------------------
; rm_quiet / rm_loud - around LRGDISK's own commands: the cable's IRQ
; masked at the PIC and the drive told not to raise it (nIEN), so the
; BIOS's handler never sees an interrupt it did not ask for;
; afterwards the status read clears whatever is pending and both are
; put back.
;---------------------------------------------------------------------
rm_quiet:
        push    ax
        push    cx
        push    dx
        in      al,0A1h
        mov     byte ptr ds:[rv_pic-rbase],al
        movzx   cx,byte ptr ds:[rc_cable-rbase]
        add     cx,6
        bts     ax,cx
        out     0A1h,al
        mov     dx,word ptr ds:[rc_ctl-rbase]
        mov     al,0Ah
        out     dx,al
        pop     dx
        pop     cx
        pop     ax
        ret

rm_loud:
        push    ax
        push    dx
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_STATUS
        in      al,dx
        mov     dx,word ptr ds:[rc_ctl-rbase]
        mov     al,08h
        out     dx,al
        mov     al,byte ptr ds:[rv_pic-rbase]
        out     0A1h,al
        pop     dx
        pop     ax
        ret

;---------------------------------------------------------------------
; rm_delay - 400ns and more: four reads of the alternate status
;---------------------------------------------------------------------
rm_delay:
        push    ax
        push    dx
        mov     dx,word ptr ds:[rc_ctl-rbase]
        in      al,dx
        in      al,dx
        in      al,dx
        in      al,dx
        pop     dx
        pop     ax
        ret

;---------------------------------------------------------------------
; rm_bsy - wait up to a second for BSY to go, by the alternate status
; (which leaves an interrupt request alone).  AL = status; CF if it
; stayed.
;---------------------------------------------------------------------
rm_bsy:
        push    cx
        push    dx
        push    es
        xor     cx,cx
        mov     es,cx
        mov     cx,19
        mov     dx,word ptr ds:[rc_ctl-rbase]
rb_tick:
        mov     ah,byte ptr es:[46Ch]
rb_poll:
        in      al,dx
        test    al,ST_BSY
        jz      rb_done
        cmp     ah,byte ptr es:[46Ch]
        je      rb_poll
        loop    rb_tick
        stc
        jmp     rb_out
rb_done:
        clc
rb_out:
        pop     es
        pop     dx
        pop     cx
        ret

;---------------------------------------------------------------------
; rm_settle - two timer ticks
;---------------------------------------------------------------------
rm_settle:
        push    es
        xor     ax,ax
        mov     es,ax
        mov     cx,2
rst_tick:
        mov     al,byte ptr es:[46Ch]
rst_wait:
        cmp     al,byte ptr es:[46Ch]
        je      rst_wait
        loop    rst_tick
        pop     es
        ret

;---------------------------------------------------------------------
; rm_irqtest - does the cable's interrupt (IRQ 14 or 15) reach the
; processor?  Its vector (INT 76h or 77h) is pointed at a stub that
; notes the interrupt and ends it at the PIC, the IRQ is unmasked, and
; unit rc_unit is given a read of its first sector with its interrupt
; on: the request is raised as the data becomes ready, before the
; status register is read.  CF if it did not come, the mask put back
; as it was; if it came the IRQ is left unmasked - a BIOS that polls
; (SeaBIOS, on the second cable) leaves it masked, and Windows takes
; the mask as it finds it.  rc_val = the status seen.
;---------------------------------------------------------------------
rm_irqtest:
        push    es
        movzx   bx,byte ptr ds:[rc_cable-rbase]
        add     bx,76h
        shl     bx,2                    ; BX = the vector's slot
        xor     ax,ax
        mov     es,ax
        mov     eax,dword ptr es:[bx]
        mov     dword ptr ds:[rv_oldvec-rbase],eax
        cli
        mov     word ptr es:[bx],rm_irqstub-rbase
        mov     word ptr es:[bx+2],cs
        push    cs
        pop     es
        mov     byte ptr ds:[rv_irqhit-rbase],0
        in      al,0A1h
        mov     byte ptr ds:[rv_pic-rbase],al
        movzx   cx,byte ptr ds:[rc_cable-rbase]
        add     cx,6
        btr     ax,cx
        out     0A1h,al
        sti

        mov     dx,word ptr ds:[rc_ctl-rbase] ; the drive's interrupt on
        mov     al,08h
        out     dx,al
        mov     al,byte ptr ds:[rc_unit-rbase]
        shl     al,4
        or      al,0A0h
        mov     dx,word ptr ds:[rc_base-rbase]
        add     dx,R_DRVHD
        out     dx,al
        call    rm_delay
        call    rm_bsy
        jc      rit_fail
        mov     dx,word ptr ds:[rc_base-rbase]
        inc     dx                      ; features
        xor     al,al
        out     dx,al
        inc     dx                      ; count
        mov     al,1
        out     dx,al
        inc     dx                      ; sector 1 of cylinder 0, head 0:
        out     dx,al                   ; the first, whatever the geometry
        inc     dx
        xor     al,al
        out     dx,al
        inc     dx
        out     dx,al
        add     dx,R_CMD-R_CYLHI
        mov     al,ATA_READ
        out     dx,al
        call    rm_delay
        call    rm_bsy                  ; (the alternate status, which
        jc      rit_fail                ; leaves the request alone)
        test    al,ST_ERR
        jnz     rit_fail
        test    al,ST_DRQ
        jz      rit_fail
        mov     cx,4000                 ; the request is up: a moment
rit_wait:                               ; for it to come in
        cmp     byte ptr ds:[rv_irqhit-rbase],0
        jne     rit_came
        mov     dx,word ptr ds:[rc_ctl-rbase]
        in      al,dx
        loop    rit_wait
rit_came:
        mov     di,rv_buf2-rbase        ; the data, and the status, which
        mov     cx,256                  ; drops the request
        mov     dx,word ptr ds:[rc_base-rbase]
        rep     insw
        add     dx,R_STATUS
        in      al,dx
        mov     byte ptr ds:[rc_val-rbase],al
        cmp     byte ptr ds:[rv_irqhit-rbase],0
        je      rit_fail
        call    rit_restore
        pop     es
        clc
        ret
rit_fail:
        mov     byte ptr ds:[rc_val-rbase],al
        mov     dx,word ptr ds:[rc_base-rbase] ; (whatever is pending,
        add     dx,R_STATUS             ; dropped)
        in      al,dx
        call    rit_restore
        mov     al,byte ptr ds:[rv_pic-rbase] ; the mask as it was
        out     0A1h,al
        pop     es
        stc
        ret

rit_restore:                            ; the vector back, interrupts off
        cli
        push    es
        xor     ax,ax
        mov     es,ax
        mov     eax,dword ptr ds:[rv_oldvec-rbase]
        mov     dword ptr es:[bx],eax
        pop     es
        sti
        ret

rm_irqstub:                             ; the interrupt: noted, ended
        push    ax
        mov     byte ptr cs:[rv_irqhit-rbase],1
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ax
        iret

;---------------------------------------------------------------------
; rm_hang - what INT 2Fh AH=13h holds for the moment between the two
; swaps; should anything call it, the ROM
;---------------------------------------------------------------------
rm_hang:
        jmp     dword ptr cs:[rv_rom13-rbase]

;---------------------------------------------------------------------
; rm_cleanup - InDOS back down and the end of the detection said,
; once
;---------------------------------------------------------------------
rm_cleanup:
        cmp     byte ptr ds:[rv_detect-rbase],0
        je      rcu_out
        mov     byte ptr ds:[rv_detect-rbase],0
        les     bx,dword ptr ds:[rv_indos-rbase]
        dec     byte ptr es:[bx]
        mov     ax,1607h
        mov     bx,0010h
        mov     cx,2                    ; detection ends
        int     2Fh
        push    cs
        pop     ds
        push    cs
        pop     es
rcu_out:
        ret

;---------------------------------------------------------------------
; rm_profile - a SYSTEM.INI [386Enh] boolean through the loader: DI =
; the key, ECX = the default.  CX = the answer.
;---------------------------------------------------------------------
rm_profile:
        push    ds
        push    es
        mov     ax,3                    ; Get_Profile_Boolean
        xor     si,si                   ; [386Enh]
        call    dword ptr ds:[rv_svc-rbase]
        pop     es
        pop     ds
        ret

;---------------------------------------------------------------------
; The report: rm_puts (SI -> text), rm_putc (AL), rm_hex (EAX, CL
; digits), rm_dec (AX) append to rv_text; rm_show writes it to the
; screen, rm_file to LRGDISK.TXT in the boot drive's root.
;---------------------------------------------------------------------
rm_puts:
        push    ax
rps_next:
        lodsb
        or      al,al
        jz      rps_done
        call    rm_putc
        jmp     rps_next
rps_done:
        pop     ax
        ret

rm_putc:
        push    di
        mov     di,word ptr ds:[rv_tlen-rbase]
        cmp     di,RV_TEXTMAX
        jae     rpc_full
        mov     byte ptr ds:[di+(rv_text-rbase)],al
        inc     di
        mov     word ptr ds:[rv_tlen-rbase],di
rpc_full:
        pop     di
        ret

rm_hex:
        push    eax
        push    cx
        push    edx
        mov     edx,eax
        movzx   cx,cl
rhx_digit:
        push    cx
        dec     cx
        shl     cx,2                    ; this digit's lowest bit
        mov     eax,edx
        shr     eax,cl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     rhx_put
        add     al,'A'-'0'-10
rhx_put:
        call    rm_putc
        pop     cx
        loop    rhx_digit
        pop     edx
        pop     cx
        pop     eax
        ret

rm_dec:                                 ; AX
        push    eax
        movzx   eax,ax
        call    rm_dec32
        pop     eax
        ret

rm_dec32:                               ; EAX
        push    eax
        push    cx
        push    edx
        push    ebx
        xor     cx,cx
        mov     ebx,10
rdc_div:
        xor     edx,edx
        div     ebx
        push    dx
        inc     cx
        or      eax,eax
        jnz     rdc_div
rdc_put:
        pop     ax
        add     al,'0'
        call    rm_putc
        loop    rdc_put
        pop     ebx
        pop     edx
        pop     cx
        pop     eax
        ret

rm_show:
        mov     ah,40h
        mov     bx,2
        mov     cx,word ptr ds:[rv_tlen-rbase]
        mov     dx,rv_text-rbase
        int     21h
        ret

rm_file:
        mov     ax,3305h                ; the boot drive
        int     21h
        jc      rf_c
        or      dl,dl
        jnz     rf_drive
rf_c:
        mov     dl,3
rf_drive:
        add     dl,'A'-1
        mov     byte ptr ds:[rv_fname-rbase],dl
        mov     ah,3Ch
        xor     cx,cx
        mov     dx,rv_fname-rbase
        int     21h
        jc      rf_out
        mov     bx,ax
        mov     ah,40h
        mov     cx,word ptr ds:[rv_tlen-rbase]
        mov     dx,rv_text-rbase
        int     21h
        mov     ah,3Eh
        int     21h
rf_out:
        ret

;---------------------------------------------------------------------
; Real-mode data
;---------------------------------------------------------------------
rv_svc          dd      0               ; the loader's services
rv_rom13        dd      0               ; the ROM's INT 13h
rv_dos13        dd      0               ; what DOS's disk driver calls
rv_indos        dd      0
rv_tlen         dw      0
rv_answer       dd      0               ; the reference data
rv_bm           dw      0, 0            ; each cable's engine port, 0 none
rv_ndrv         db      0
rv_detect       db      0               ; InDOS raised, detection said
rv_report       db      0
rv_dma          db      0               ; LRGDiskDMA
rv_serial       db      0               ; one cable at a time
rv_block        db      0               ; LRGDiskBlockMode
rv_poll         db      0               ; LRGDiskPoll
rv_io32         db      0               ; LRGDisk32BitIO
rv_pciide       db      0               ; the controller is a PCI one
rv_no32         db      0               ; ...and a CMD640
rv_pic          db      0
rv_irqhit       db      0               ; rm_irqtest: it came
rv_oldvec       dd      0               ; ...and the vector it replaced
rv_lastbus      db      0
rv_cbl          db      0, 0            ; each cable: bit 0, one of ours
                                        ; is on it
rv_pst          dd      0               ; rm_present's status, its status
                                        ; after IDENTIFY, the cylinders
rv_units        db      4*UR_SIZE dup (0) ; the unit records (UR_*)

rc_t            dd      0               ; the sector being tried
rc_total        dd      0               ; sectors the BIOS reaches
rc_val          dd      0               ; what the failing check saw
rc_tf           db      4 dup (0)       ; +3 to +6
rc_maxcyl       dw      0               ; AH=08h's highest cylinder
rc_heads        dw      0
rc_tcyl         dw      0               ; the table's cylinders
rc_base         dw      0               ; the cable's ports
rc_ctl          dw      0
rc_spt          db      0
rc_s            db      0
rc_drive        db      0
rc_unit         db      0
rc_cable        db      0
rc_mode         db      0               ; 0 CHS, 1 LBA
rc_48           db      0               ; ...and LBA48, past 28 bits
rc_ext          db      0               ; rm_ownread: a 48-bit command
rc_lbaok        db      0               ; IDENTIFY says it has LBA
rc_step         db      0
rc_how          db      0               ; HOW_*: what the BIOS's reads fit
rc_xfer         db      0               ; the DMA mode selected, as BP_XFER
rc_mmax         db      0               ; IDENTIFY: the most sectors a block
rc_mcur         db      0               ; ...and the size set now, 0 none
rc_mult         db      0               ; the size PIO will use, 0 none
rc_w48          db      0               ; IDENTIFY: doubleword I/O offered
rc_cap          dd      0               ; IDENTIFY: LBA sectors
rc_dheads       dw      0               ; IDENTIFY: the drive's geometry
rc_dspt         dw      0
rc_cheads       dw      0               ; IDENTIFY: what it is set to
rc_cspt         dw      0
rc_list         dd      4 dup (0)       ; the four test sectors
rc_howmsg       dw      0               ; the report's lines for the way
rc_endmsg       dw      0               ; chosen, the end check, and
rc_dmamsg       dw      0               ; the engine
rc_blkmsg       dw      0               ; ...and for PIO's blocks, before
rc_blkmsg2      dw      0               ; and after the size, and the
rc_32msg        dw      0               ; data's width
rv_dap          db      16 dup (0)      ; an INT 13h extensions packet

k_32bit         db      '32BITDISKACCESS', 0
k_report        db      'LRGDISKREPORT', 0
k_dma           db      'LRGDISKDMA', 0
k_serial        db      'LRGDISKSERIAL', 0
k_block         db      'LRGDISKBLOCKMODE', 0
k_poll          db      'LRGDISKPOLL', 0
k_io32          db      'LRGDISK32BITIO', 0
rv_fname        db      'C:\LRGDISK.TXT', 0

m_hello         db      'LRGDISK 1.3', 13, 10, 0
m_oldwin        db      'Windows 3.1 or later is needed.', 13, 10, 0
m_nodisk        db      'The BIOS reports no hard disk.', 13, 10, 0
m_no2f          db      'DOS does not answer INT 2Fh AH=13h.', 13, 10, 0
m_hooked        db      'A program between DOS and the BIOS handles the '
                db      'hard disks (INT 13h).', 13, 10, 0
m_bm_off        db      'LRGDiskDMA=off: PIO.', 13, 10, 0
m_bm_none       db      'No PCI bus-master IDE controller: PIO.', 13, 10, 0
m_bm            db      'Bus-master IDE at port ', 0
m_bm2           db      'h (PCI bus ', 0
m_bm2b          db      ' device ', 0
m_bm3           db      ')', 0
m_bm_nat1       db      '; the first cable is in native mode, not at '
                db      '1F0h', 0
m_bm_nat2       db      '; the second cable is in native mode, not at '
                db      '170h', 0
m_rz1000        db      'PC-Tech RZ1000: its read-ahead is switched off (it '
                db      'corrupts reads).', 13, 10, 0
m_cmd640        db      'CMD640: its read-ahead is switched off (it corrupts '
                db      'reads).', 13, 10, 0
m_serial        db      'The two cables are used one at a time.', 13, 10, 0
m_other         db      'Unit ', 0
m_cable1        db      ' of the first cable', 0
m_cable2        db      ' of the second cable', 0
m_other_ata     db      ' is a disk LRGDISK leaves to the BIOS (', 0
m_other_atapi   db      ' is a CD-ROM drive or other ATAPI device (', 0
m_other2        db      ').  DOS drivers share the cable with LRGDISK.'
                db      13, 10, 0
m_none          db      'No hard disk passed the checks.', 13, 10, 0
m_off           db      '32-bit disk access is off.', 13, 10, 0
m_on            db      '32-bit disk access is on.', 13, 10, 0
m_disk          db      'BIOS disk ', 0
m_disk2         db      'h: ', 0
m_table         db      ', the table says ', 0
m_notable       db      ', no table', 0
m_drive         db      '  the drive, unit ', 0
m_drive2        db      ': ', 0
m_haslba        db      ' sectors, LBA', 13, 10, 0
m_has48         db      ' sectors, LBA and LBA48', 13, 10, 0
m_nolba         db      ' sectors, no LBA', 13, 10, 0
m_how_lba       db      '  read by LBA, as the BIOS reads it', 0
m_how_same      db      '  read by LBA; the BIOS', 27h, 's CHS is the '
                db      'drive', 27h, 's own', 0
m_how_large     db      '  read by LBA; the BIOS uses the drive', 27h, 's '
                db      'own CHS', 0
m_how_chs       db      '  read by CHS, as the BIOS reads it', 0
m_end_ok        db      '; the last sector too, through the BIOS', 27h
                db      's extensions', 13, 10, 0
m_end_28        db      '; the BIOS stops at 128GB, its last sector '
                db      'there checked', 13, 10, 0
m_end_noext     db      '; the BIOS has no extensions to reach the last '
                db      'sector with', 13, 10, 0
m_dma_off       db      '  PIO: LRGDiskDMA=off', 0
m_dma_none      db      '  PIO: no DMA mode is selected on the drive', 0
m_dma_noeng     db      '  PIO: no bus-master engine for this cable', 0
m_dma_udma      db      '  bus-master DMA, Ultra DMA mode ', 0
m_dma_mw        db      '  bus-master DMA, multiword DMA mode ', 0
m_poll_off      db      'LRGDiskPoll=off: an interrupt for every PIO block.'
                db      13, 10, 0
m_blk_in        db      '  PIO in blocks of ', 0
m_blk_was       db      ' sectors, as the drive was set', 0
m_blk_set       db      ' sectors, set by LRGDISK', 0
m_blk_off       db      '  PIO a sector at a time: LRGDiskBlockMode=off', 0
m_blk_none      db      '  PIO a sector at a time: the drive has no block '
                db      'mode', 0
m_blk_one       db      '  PIO a sector at a time: the drive is set to '
                db      'blocks of one sector', 0
m_blk_refused   db      '  PIO a sector at a time: the drive refused a '
                db      'block size', 0
m_32_on         db      '  PIO data 32 bits at a time', 0
m_32_off        db      '  PIO data 16 bits at a time: LRGDisk32BitIO=off', 0
m_32_cmd        db      '  PIO data 16 bits at a time: a CMD640 controller', 0
m_32_none       db      '  PIO data 16 bits at a time: no PCI controller, '
                db      'no 32-bit I/O in the drive', 0
m_32_bad        db      '  PIO data 16 bits at a time: a 32-bit read came '
                db      'back different', 0
m_check         db      '  check ', 0
m_value         db      ' failed (', 0
m_close         db      '): ', 0
m_crlf          db      13, 10, 0

; why each check fails, in step order
rv_whys         dw      w_1-rbase, w_2-rbase, w_3-rbase, w_4-rbase
                dw      w_5-rbase, w_6-rbase, w_7-rbase, w_8-rbase
                dw      w_9-rbase, w_10-rbase, w_11-rbase, w_12-rbase
                dw      w_13-rbase, w_14-rbase, w_15-rbase
w_1             db      'INT 13h AH=08h failed', 0
w_2             db      'the BIOS table', 27h, 's heads differ', 0
w_3             db      'the BIOS table', 27h, 's sectors differ', 0
w_4             db      'the BIOS table', 27h, 's cylinders are not 1 to '
                db      '3 past the highest', 0
w_5             db      'the BIOS', 27h, 's read of the first sector '
                db      'failed, went to neither IDE cable, or went to '
                db      'a unit another disk is', 0
w_6             db      'the cable', 27h, 's interrupt did not reach the '
                db      'processor', 0
w_7             db      'the drive did not answer IDENTIFY', 0
w_8             db      'a BIOS read failed', 0
w_9             db      'the BIOS read did not go to this drive', 0
w_10            db      'the BIOS addressed the drive in a way LRGDISK '
                db      'does not', 0
w_11            db      'the BIOS used LBA on a drive without it', 0
w_12            db      'LRGDISK', 27h, 's own read failed', 0
w_13            db      'LRGDISK', 27h, 's own read brought back other '
                db      'data', 0
w_14            db      'the BIOS', 27h, 's extended read of the last '
                db      'sector failed', 0
w_15            db      'the BIOS', 27h, 's extended read of the last '
                db      'sector went somewhere else', 0

rv_text         db      RV_TEXTMAX dup (0)
rv_buf1         db      512 dup (0)     ; the BIOS's copy
rv_buf2         db      512 dup (0)     ; LRGDISK's copy; IDENTIFY's
_RCODE  ENDS

        END     rm_entry
