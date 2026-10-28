;=====================================================================
; PMDOS.386 - the Windows-for-Workgroups-3.11 bridge VxD
;
; When WfW 3.11 runs in 386 enhanced mode the PM-DOS kernel is PARKED:
; its protected mode is gone, WIN386 owns CR0/CR3/GDT/IDT, and RMDOS is
; the DOS underneath (DOS\PARK.INC, DOS\WINDOWS.INC).  This VxD is the
; way back in - the "thunk home" the kernel otherwise cannot have.  It
; runs at ring 0 inside WIN386's VMM and, through a trampoline that
; switches CR3 to the parked kernel's own identity-mapped page
; directory, calls the kernel's loaded 32-bit drivers (the NIC, the
; TCP/IP stack, the SMB redirector) as subroutines.  Those drivers were
; written to "the bridge's rule" - no INT 21h, no waiting - precisely so
; they can be called this way (INC\NDRV.INC, DOS\NDLOAD.INC, brg_entry).
;
; HOW THE SWITCH IS SAFE.  The trampoline runs from a page allocated in
; the VMM global V86 data area, which is BELOW 1MB and therefore mapped
; linear==physical under both WfW's system VM and the kernel's identity
; map.  That is the one place the instruction after each 'mov cr3' is
; fetched correctly across the switch.  The page also holds the scratch
; stack and the bounce buffer, so no kernel low memory is borrowed.  The
; kernel publishes brg_entry (and a signature) in its globals block,
; which this VxD reads 1:1 (PMIO's 4K stub, globals at linear 1F00h,
; survives the park - proved).
;
; STAGE A (the bridge) is proved: a real driver call (NIC_INFO) through
; the trampoline hands back the parked kernel's NIC driver's MAC and
; name, and WfW keeps running.  Device_Init does that once as a
; self-test.
;
; STAGE B begins here: a V86 API entry (DDB_V86_API_Proc), which a
; 16-bit program under enhanced mode reaches with INT 2Fh AX=1684h,
; BX=504Dh.  It is the entry RMDOS will call to put the parked kernel's
; network letters and USB disks in front of a DOS box.  B1 proves the
; path whole: the API performs the same NIC_INFO bridge call on behalf
; of the caller and copies the answer into the caller's buffer.
;
; An LE VxD, laid out by wlink's "format windows vxd" as the DDK's
; LINK386 does; the segment classes are WDCTRL.DEF's, as CMD\LRGDISK
; does it.  No VMM.INC: the descriptor block, the one VMM service used
; and its flags are the DDK's numbers, hand-built.
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

_LGROUP GROUP   _LTEXT, _LDATA
_IGROUP GROUP   _ITEXT, _IDATA

VXDCALL MACRO   svc
        int     20h
        dd      svc
        ENDM

;---------------------------------------------------------------------
; The DDK's numbers (VMM.INC), the handful this file needs.
;---------------------------------------------------------------------
PMDOS_DEVICE_ID         EQU     504Dh           ; "PM" - this VxD's device ID
PMDOS_DDK_VERSION       EQU     030Ah
UNDEFINED_INIT_ORDER    EQU     80000000h
; The NDIS netcard init-order slot.  ndis.386 inits at 54002000h, the in-box
; MACs (NE2000.386) at 54004000h, netbeui.386 at 54006000h.  Lower inits
; earlier, so the bridge MUST register its adapter here - before netbeui.386's
; Device_Init bind pass - or NetBEUI finds no adapter and nothing binds.
NETCARD_INIT_ORDER      EQU     54004000h

SYS_CRITICAL_INIT       EQU     0
DEVICE_INIT             EQU     1

; _Allocate_Global_V86_Data_Area (ordinal 0A8h in VMM = device 0001),
; verified by counting VMM.INC's service table against _PageAllocate's
; known 00010053h.  EAX = bytes, EBX = flags -> EAX = ring-0 linear
; address below 1MB (0 on failure).  An init-only (VMM_ICODE) service.
S_ALLOC_GV86            EQU     000100A8h
GVDA_PAGEALIGN          EQU     00000008h
GVDA_ZEROINIT          EQU     00000200h

; Set_Global_Time_Out (VMM device 0001, ordinal 3Ch): EAX = milliseconds,
; ESI = the callback, EDX = reference data -> the callback is entered once,
; at interrupt time, EBX = the current VM.  A runtime service (not init-
; only).  The same number, and the same re-arm-itself use, as LRGDISK.386.
S_SET_GLOBAL_TIME_OUT   EQU     0001003Ch

; VPICD (device 0003): the handful BA1b needs to take the card's receive
; line and drive the parked stack from it.  The same numbers LRGDISK.386
; uses in this tree (CMD\LRGDISK), confirmed working there.  Virtualize
; takes EDI -> a VPICD_IRQ_Descriptor and answers EAX = a handle, CF set
; if the line is already somebody's; the others take EAX = the handle.
S_VPICD_VIRTUALIZE_IRQ  EQU     00030001h
S_VPICD_CLEAR_INT_REQ   EQU     00030003h
S_VPICD_PHYS_EOI        EQU     00030004h
S_VPICD_PHYS_UNMASK     EQU     00030009h

; VMM again: the V86 INT 13h hook (BC2's cure for the splash hang).  A
; stick is SeaBIOS's INT 13h drive 81h, but this kernel owns its UHCI, so
; the BIOS can no longer read 81h - WfW's boot-sector probe of it gets
; zeroes and divides CHS by a zero geometry.  Hook_V86_Int_Chain puts the
; VxD in front of every V86 INT 13h; for a USB disk it answers out of the
; kernel's own USB stack (blk_bridge) and eats the call with Simulate_Iret,
; so the BIOS is never asked.  Ordinals from the DDK VMM.INC (386/INT13
; is the model), the same way the services above were counted.
S_HOOK_V86_INT_CHAIN    EQU     00010041h
S_SIMULATE_IRET         EQU     00010047h

E9_PORT                 EQU     0E9h

;---------------------------------------------------------------------
; Client_Reg_Struc offsets (VMM.INC).  When the V86 API proc is called,
; EBP -> this structure of the calling VM's registers; a 16-bit client's
; registers are the low words of the dword fields, and its flags are the
; low word of Client_EFlags, where bit 0 is the carry the caller reads.
;---------------------------------------------------------------------
C_EDI                   EQU     00h
C_ESI                   EQU     04h
C_EBP                   EQU     08h
C_EBX                   EQU     10h
C_EDX                   EQU     14h
C_ECX                   EQU     18h
C_EAX                   EQU     1Ch
C_EIP                   EQU     24h
C_CS                    EQU     28h
C_EFLAGS                EQU     2Ch
C_ESP                   EQU     30h
C_SS                    EQU     34h
C_ES                    EQU     38h
C_DS                    EQU     3Ch

;---------------------------------------------------------------------
; The parked kernel's low-memory landmarks (INC\PMDOS.INC: IO_BASE and
; GV_OFF).  PMIO loads at IO_BASE = 1000h, so its globals block sits
; below 1MB, 1:1 under WfW's system VM.  This VxD reads these at their
; own linear address; the address must be in a register, as wasm treats
; brackets round a pure constant as the immediate, not a memory load.
;---------------------------------------------------------------------
KERN_IOBASE             EQU     1000h
KERN_GVBASE             EQU     KERN_IOBASE + 0F00h     ; 1F00h
KL_GV_PGDIR             EQU     KERN_GVBASE + 14h       ; dd - kernel CR3 (00180000)
KL_GV_DOSSIZE           EQU     KERN_GVBASE + 04h       ; dd - bytes of PMDOS.SYS
KL_GV_BRGSIG            EQU     KERN_GVBASE + 0E0h      ; dd - BRG_SIGVAL when supported
KL_GV_BRGENTRY          EQU     KERN_GVBASE + 0E4h      ; dd - brg_entry
BRG_SIGVAL              EQU     42475250h               ; 'PRGB', PMDOS.INC's value

; The contract brg_entry answers (INC\NDRV.INC, NIC.INC, SOCKET.H)
NDTF_NIC                EQU     1               ; the card
NDTF_STACK              EQU     2               ; the TCP/IP stack
NIC_INFO                EQU     0
NIC_WAKE                EQU     5               ; bring the parked card up, polled
NIC_WAKE_IRQ            EQU     6               ; ...or with its interrupt live
NIC_NOIRQ               EQU     0FFh            ; NICI_IRQ when the card is polled
NICI_LEN                EQU     52
NICI_MAC                EQU     0
NICI_IRQ                EQU     12
NICI_NAME               EQU     20

; Stage D - the kernel NIC's raw L2 transport (DRV\NETCORE.C nd_call), the
; functions the NDIS MAC face drives.  NIC_INFO/NIC_WAKE above are the same
; call with a different function number.
NIC_SEND                EQU     1               ; TX one Ethernet frame (card_send)
NIC_RECV                EQU     2               ; pop one received frame off the queue
NIC_SERVICE             EQU     4               ; drain the HW ring into the queue
NIC_MAXFRAME            EQU     1514
NIC_HDRLEN              EQU     14              ; dest(6) + source(6) + len/type(2)

; The stack's functions the bridge drives (INC\SOCKET.H)
SOCK_IFSTAT            EQU     0               ; the interface and its address
SOCK_RECV               EQU     7               ; receive (Stage C: trace shows the data)
SOCK_STATE              EQU     8               ; a socket's state (polled, so not traced)
SOCK_RESOLVE            EQU     9               ; a name to an address (polled)
SOCK_PUMP               EQU     11              ; one step of the parked stack
SOCK_RXFOREIGN          EQU     14              ; collect one non-IP/ARP frame (NetBEUI)
SOCKIF_LEN             EQU     60
SIF_UP                 EQU     26              ; SOCK_IF: 1 once an address is held
SOCKRES_LEN             EQU     136
SR_IP                   EQU     0               ; SOCK_RES fields
SR_STATE                EQU     4
SR_NAME                 EQU     8
NET_PENDING             EQU     0               ; a lookup's state
NET_DONE                EQU     1
NET_FAIL                EQU     2

; The redirector (NDTF_REDIR), which the bridge drives for a net drive -
; BA2.  The op is ORed with REDIR_RESUME so the parked SMBFS driver runs
; it on its worker fibre and yields whenever a socket wait has no data;
; REDIR_PENDING in the request's status word is "poll me again" (the
; bridge lets Windows run between polls, so the tick and the card's
; interrupt advance the parked stack).  Offsets and values are
; INC\REDIR.INC's (the redir_req struct in INC\REDIR.H).
NDTF_REDIR              EQU     4
REDIR_MOUNT             EQU     1
REDIR_FINDFIRST         EQU     3
REDIR_RESUME            EQU     1000h
REDIR_PENDING           EQU     0FFh
NR_OP                   EQU     0               ; the REDIR_* op the caller wants run
NR_DRIVE                EQU     4
NR_RESULT               EQU     28              ; bytes a read moved
NR_BUFPTR               EQU     48              ; a read's destination, flat linear
REDIR_READ              EQU     6               ; so the trace can dump what a read fetched
REDIR_WRITE             EQU     7
NR_STATUS               EQU     12
NR_COUNT                EQU     24              ; bytes a read or write moves
NR_SIZELO               EQU     32
NR_ATTR                 EQU     40
NR_SPARE                EQU     60
NR_PATH                 EQU     64              ; char[260]
NR_PATTERN              EQU     324             ; char[16]
NR_NAME                 EQU     340
NR_SERVER               EQU     356
NR_SHARE                EQU     420
NR_USER                 EQU     500
NR_PASS                 EQU     564
NR_DOMAIN               EQU     628
NR_SIZE                 EQU     692
REDIR_SRCH              EQU     16h             ; the find's search attribute (dirs too)
PD_SELFDRIVE            EQU     13              ; the self-test's net-drive slot (N:)

; The block bridge (USB disks while parked).  NDTF_BLOCK is a sentinel to
; brg_entry, not a loaded driver; the block_req (BR_*) matches INC\PMDOS.INC.
NDTF_BLOCK              EQU     8
BR_OP                   EQU     0               ; 0 = read, 1 = write
BR_DRIVE                EQU     4               ; the block drive number (USB_DRV0 up)
BR_LBA                  EQU     8
BR_COUNT                EQU     12
BR_STATUS               EQU     16              ; out: 0, or an INT 13h status byte
BR_RESULT               EQU     20              ; out: sectors moved
BR_DATA                 EQU     32              ; the sector payload
BR_SIZE                 EQU     32 + 512
PD_USBDRIVE             EQU     82h             ; USB_DRV0: the first USB stick's unit

;---------------------------------------------------------------------
; Stage D - the WfW 3.11 NDIS 3.0 wrapper (NDIS.386, device 0028h).  This
; VxD registers with it as a native 32-bit MAC so WfW's own protocols
; (NetBEUI) bind to the kernel-owned card.  The service numbers, the call
; form and the structure offsets were recovered by disassembling the
; in-box NE2000.386/NDIS.386 (there is no DDK here).  A service that takes
; stack arguments and returns a value is reached by CALLing a thunk whose
; body is 'int 20h; dd id' - VMM rewrites the thunk to a jump to the
; service, which returns to the thunk's caller.  8000h is the dynamic-link
; "call form / do not fault if the device is absent" flag.
;---------------------------------------------------------------------
NDIS_DYNLINK            EQU     00288000h       ; (0028h<<16) OR 8000h, ORed with svc
NDIS_GET_VERSION        EQU     0               ; or eax,300h; clc; ret (presence check)
NDIS_REGISTER_MAC       EQU     31h             ; (&handle, descriptor) -> a MAC handle
NDIS_DEREGISTER         EQU     32h             ; undo a failed registration
NDIS_REGISTER_ADAPTER   EQU     33h             ; register MAC characteristics (&status,&ctx,h,h2,&chartable,size) -> ctx = the char handle
NDIS_REGISTER_ADAP2     EQU     35h             ; register the bindable adapter instance, called from AddAdapter (&out,charh,ctx,cfg,&name,0)
NDIS_OPEN_CONFIG        EQU     05h             ; NdisOpenConfiguration(&status,&cfgh,ConfigHandle) - open the card's PROTOCOL.INI section
NDIS_CLOSE_CONFIG       EQU     07h             ; NdisCloseConfiguration(cfgh)
NDIS_QUERY_PACKET       EQU     1Bh             ; (pkt,0,0,&firstbuf,0)
NDIS_QUERY_BUFFER       EQU     21h             ; (buf,&vaddr,&len)
NDIS_NEXT_BUFFER        EQU     27h             ; (buf,&nextbuf)
NDIS_IND_RECEIVE        EQU     3Dh             ; EthFilterIndicateReceive (8 args)
NDIS_IND_RECV_DONE      EQU     3Eh             ; EthFilterIndicateReceiveComplete (1 arg)

; NDIS_STATUS values (recovered from the handlers)
NDIS_SUCCESS            EQU     0
NDIS_FAILURE            EQU     0C0000001h
NDIS_RESOURCES          EQU     0C000009Ah
NDIS_NOT_SUPPORTED      EQU     0C00000BBh
NDIS_BAD_LENGTH         EQU     0C0010014h
NDIS_INVALID_OID        EQU     0C0010017h
NDIS_CLOSING            EQU     0C0010002h
NDIS_RESET_IN_PROGRESS  EQU     0C001000Dh
NDIS_UNSUPPORTED_MEDIA  EQU     0C0010019h

; MAC characteristics table (NDIS30_MAC_CHARACTERISTICS, 36h bytes).  Ten
; handler pointers, then an NDIS_STRING for the MAC's name.
MACC_MAJOR              EQU     0               ; byte = 3
MACC_MINOR              EQU     1               ; byte = 0
MACC_OPENADAPTER        EQU     6
MACC_CLOSEADAPTER       EQU     0Ah
MACC_SEND               EQU     0Eh
MACC_TRANSFERDATA       EQU     12h
MACC_RESET              EQU     16h
MACC_REQUEST            EQU     1Ah
MACC_QUERYSTATS         EQU     1Eh
MACC_UNLOADMAC          EQU     22h
MACC_ADDADAPTER         EQU     26h
MACC_REMOVEADAPTER      EQU     2Ah
MACC_NAME_LEN           EQU     2Eh             ; word
MACC_NAME_MAXLEN        EQU     30h             ; word
MACC_NAME_BUF           EQU     32h             ; dword
MACC_SIZE               EQU     36h

; NDIS_REQUEST offsets (the struct MacRequest receives at [ebp+0Ch])
NRQ_TYPE                EQU     10h             ; 0 = QueryInformation, 1 = SetInformation
NRQ_OID                 EQU     14h
NRQ_BUFFER              EQU     18h             ; InformationBuffer (flat linear)
NRQ_BUFLEN              EQU     1Ch             ; InformationBufferLength
NRQ_WRITTEN             EQU     20h             ; BytesWritten / BytesRead (out)
NRQ_NEEDED              EQU     24h             ; BytesNeeded (out)

; The OIDs this MAC answers (ndis.h values; NE2000's exact answers are
; mirrored so a known-good MAC's replies are matched)
OID_GEN_SUPPORTED_LIST          EQU 00010101h
OID_GEN_HARDWARE_STATUS         EQU 00010102h
OID_GEN_MEDIA_SUPPORTED         EQU 00010103h
OID_GEN_MEDIA_IN_USE            EQU 00010104h
OID_GEN_MAXIMUM_LOOKAHEAD       EQU 00010105h
OID_GEN_MAXIMUM_FRAME_SIZE      EQU 00010106h
OID_GEN_LINK_SPEED              EQU 00010107h
OID_GEN_TRANSMIT_BUFFER_SPACE   EQU 00010108h
OID_GEN_RECEIVE_BUFFER_SPACE    EQU 00010109h
OID_GEN_TRANSMIT_BLOCK_SIZE     EQU 0001010Ah
OID_GEN_RECEIVE_BLOCK_SIZE      EQU 0001010Bh
OID_GEN_VENDOR_DESCRIPTION      EQU 0001010Dh
OID_GEN_CURRENT_PACKET_FILTER   EQU 0001010Eh
OID_GEN_CURRENT_LOOKAHEAD       EQU 0001010Fh
OID_GEN_DRIVER_VERSION          EQU 00010110h
OID_GEN_MAXIMUM_TOTAL_SIZE      EQU 00010111h
OID_GEN_MAC_OPTIONS             EQU 00010113h
OID_802_3_PERMANENT_ADDRESS     EQU 01010101h
OID_802_3_CURRENT_ADDRESS       EQU 01010102h
OID_802_3_MULTICAST_LIST        EQU 01010103h
OID_802_3_MAXIMUM_LIST_SIZE     EQU 01010104h

; The async tick: a SOCK_PUMP every PD_TICKMS, a trace every PD_HEARTBEAT
; of them.  55 ms is the kernel's own tick, so the parked stack is driven
; at the cadence it was written for.
PD_TICKMS               EQU     55
PD_HEARTBEAT            EQU     18

; The V86 API's sub-functions (client AX)
PDAPI_NICINFO           EQU     0               ; B1 proof: NIC_INFO -> caller's buffer
PDAPI_BLOCK             EQU     2               ; BC2: one USB-disk sector read/write for
                                                ; RMDOS - the caller's block_req at ES:DX,
                                                ; the op in BR_OP, the answer in BR_STATUS.
                                                ; One bridge window does it (no PENDING);
                                                ; AX=0FFFFh + CF = the tick held the bridge,
                                                ; retry
PDAPI_REDIR             EQU     1               ; BA2: run one resumable redirector slice
                                                ; on the caller's redir_req at ES:DX; the
                                                ; op is in its NR_OP, the answer in NR_STATUS
                                                ; (REDIR_PENDING = the caller should yield
                                                ; and call again - RMDOS does, with INT 2Fh
                                                ; 1680h, so Windows and the tick run between)
PDAPI_SOCK              EQU     3               ; Stage C: one socket op on the parked
                                                ; kernel's TCP/IP stack, for WINSOCK.DLL.
                                                ; Caller BX = the SOCK_* function, CX = the
                                                ; request length, ES:DX = the request buffer
                                                ; (INC\SOCKET.H).  Every socket op is instant
                                                ; (the stack never blocks), so one bridge
                                                ; window does it; the WAITING is the caller's
                                                ; (it polls SOCK_STATE / SOCK_RECV and yields
                                                ; with INT 2Fh 1680h, so Windows and the async
                                                ; tick advance the parked stack between polls).
                                                ; Answer: EAX = what the function returns (a
                                                ; handle, a state, a byte count), CF clear; or
                                                ; CF set with AX = an error; or AX=0FFFFh + CF
                                                ; = the tick held the bridge this instant, retry
PDS_MAXLEN              EQU     0300h           ; the bounce buffer's room (PGO_RES-PGO_BUF)
PDS_IOHDR               EQU     16              ; SOCK_IO header before the data (SOCKET.H)
PD_V86TOP               EQU     110000h         ; the end of a VM's 1:1 window (1MB + HMA);
                                                ; a caller's buffer must end at or below it

; The low page's layout (offsets from the page base, which the
; trampoline keeps in EBP; the page is 4K and zero-filled).  The stub is
; first, then the saved/request cells, then a scratch stack that grows
; down from the buffer, then the bounce buffer.
PGO_STUB                EQU     000h            ; the trampoline (under 200h bytes)
PGO_WCR3                EQU     200h            ; saved WIN386 CR3
PGO_WESP                EQU     204h            ; saved WIN386 ESP
PGO_RET                 EQU     208h            ; brg_entry's answer
PGO_FLAGS               EQU     20Ch            ; brg_entry's flags (CF)
PGO_RQEAX               EQU     210h            ; request: NDTF_* flag
PGO_RQEDX               EQU     214h            ;          index
PGO_RQEBX               EQU     218h            ;          driver function
PGO_RQECX               EQU     21Ch            ;          buffer length
PGO_RQEDI               EQU     220h            ;          buffer (page+PGO_BUF)
PGO_KCR3                EQU     224h            ; the kernel's CR3
PGO_KENTRY              EQU     228h            ; brg_entry's linear address
PGO_WCR0                EQU     22Ch            ; saved WIN386 CR0 (for WP)
PGO_WGDTR               EQU     230h            ; WIN386's GDTR (6 bytes), saved
PGO_LGDTR               EQU     238h            ; GDTR for the low GDT copy
PGO_GDT                 EQU     400h            ; a copy of WIN386's GDT (PGO_GDTMAX at most)
PGO_STKTOP              EQU     0C00h           ; scratch stack top (grows down)
PGO_STKRSV              EQU     400h            ; stack room kept clear of the GDT copy
PGO_GDTMAX              EQU     PGO_STKTOP - PGO_STKRSV - PGO_GDT ; the largest GDT copied
PGO_BUF                 EQU     0C00h           ; bounce buffer (0C00h..0EFFh)
PGO_RES                 EQU     0F00h           ; a buffer that persists between
                                                ; calls, which the async tick
                                                ; polls: the BA1 lookup's SOCK_RES
; The NDIS data path needs full Ethernet frames (up to NIC_MAXFRAME = 1514),
; far bigger than PGO_BUF.  The low allocation is 2 pages (8K) and contiguous,
; so the whole second page is 1:1 too; frames live there, reached through
; pd_bridge_call's page-relative EDI (EDI = PGO_TXBUF -> page+PGO_TXBUF).
PGO_TXBUF               EQU     1000h           ; mac_send's frame (1514 bytes)
PGO_RXBUF               EQU     1800h           ; the received frame (1514 bytes)
PGO_IFS                 EQU     0F90h           ; the tick's own scratch (SOCK_IF,
                                                ; and SOCK_PUMP's ignored buffer),
                                                ; kept off PGO_BUF so the tick
                                                ; never disturbs an API call's

;=====================================================================
; LOCKED DATA - resident for the life of the system: the device
; descriptor block, and the bridge state the V86 API proc reads on
; every call (the low page's address, the kernel's CR3 and brg_entry).
;=====================================================================
_LDATA  SEGMENT
        PUBLIC  PMDOS_DDB

PMDOS_DDB       dd      0                       ; DDB_Next
                dw      PMDOS_DDK_VERSION
                dw      PMDOS_DEVICE_ID
                db      0, 1                    ; PMDOS.386 0.1
                dw      0
                db      'PMDOS   '
                dd      NETCARD_INIT_ORDER      ; init in the netcard slot, before netbeui.386
                dd      OFFSET FLAT:pd_control
                dd      OFFSET FLAT:pd_v86api   ; DDB_V86_API_Proc
                dd      OFFSET FLAT:pd_pmapi    ; DDB_PM_API_Proc (Stage C: WINSOCK.DLL)
                dd      0, 0
                dd      0
                dd      0, 0

; Bridge state, set once at Device_Init, read by the V86 API proc.
pd_pg           dd      0                       ; the low 1:1 page (trampoline + buffer)
pd_kcr3         dd      0                       ; the parked kernel's CR3
pd_kentry       dd      0                       ; brg_entry's linear address
pd_clbuf        dd      0                       ; a call's client buffer, linear
pd_socklen      dd      0                       ; a PDAPI_SOCK call's request length
pd_sockfn       dd      0                       ; a PDAPI_SOCK call's SOCK_* function
pd_sockans      dd      0                       ; a PDAPI_SOCK call's answer (for the trace)

; The async bridge's state, read and written by the tick and the API.
pd_busy         db      0                       ; a bridge call is in progress
                db      0, 0, 0
pd_pumps        dd      0                       ; SOCK_PUMP calls made (a trace count)
pd_hbctr        dd      0                       ; ticks since the last heartbeat
pd_ba1          db      0                       ; the self-test lookup: 0 idle,
                                                ; 1 outstanding, 2 finished
pd_nicirq       db      0                       ; the card's line, from NIC_INFO
                db      0, 0
pd_nich         dd      0                       ; VPICD's handle for that line (BA1b)
pd_ints         dd      0                       ; card interrupts fielded (a trace count)
pd_ba2          db      0                       ; the BA2 redirector self-test: 0 prepare the
                                                ; mount, 1 mounting, 2 prepare the find, 3
                                                ; finding, 4 done
pd_usbtest      db      0                       ; the USB sector-0 self-test: 0 not yet, 1 done
                db      0, 0
pd_usbmask      db      0                       ; bit (DL-80h) set = a USB BIOS disk the
                                                ; INT 13h hook serves (found at Device_Init)
                db      0, 0, 0
pd_i13drv       dd      0                       ; the INT 13h hook's scratch: the drive,
pd_i13lba       dd      0                       ; the absolute LBA in hand,
pd_i13cnt       dd      0                       ; sectors asked,
pd_i13done      dd      0                       ; sectors moved, and
pd_i13buf       dd      0                       ; the V86 buffer, linear

; VPICD_IRQ_Descriptor for the card's receive line (BA1b).  The IRQ word
; is patched from the card's reported line at Device_Init.  pd_nicint
; drains the card through the bridge and ends the interrupt at the
; controller; pd_niceoi mirrors LRGDISK's EOI proc and is never reached
; here (the interrupt is serviced whole, never reflected to a VM).
pd_vid          dw      0                       ; the IRQ, patched at init
                dw      0                       ; options: not shared
                dd      OFFSET FLAT:pd_nicint    ; hw-int proc
                dd      0                       ; no virtual-interrupt procedure
                dd      OFFSET FLAT:pd_niceoi    ; EOI proc
                dd      0                       ; mask-change proc
                dd      0                       ; IRET proc
                dd      500                     ; ms, as LRGDISK uses

pd_tifup        db      13, 10, 'PMDOS.386: BA1 interface up, looking up example.com, pumps=', 0
pd_tdone        db      13, 10, 'PMDOS.386: BA1 name lookup finished, pumps=', 0
pd_tstate       db      ' state=', 0
pd_tip          db      ' ip=', 0
pd_thb          db      13, 10, 'PMDOS.386: tick heartbeat, pumps=', 0
pd_tints        db      ' ints=', 0

pd_rv86         db      13, 10, 'PMDOS.386: V86 API call', 0
pd_rok          db      ' ok, answer=', 0
pd_rmac         db      ' MAC=', 0
pd_rfail        db      ' - no answer (bridge not ready or bad function)', 13, 10, 0
pd_rcrlf        db      13, 10, 0
pd_msgscrit     db      13, 10, 'PMDOS.386: V86 INT 13h hooked at Sys_Critical_Init', 13, 10, 0

; USB-over-bridge self-test (step 7, Stage B USB): read the parked kernel's
; USB stick's sector 0 through the bridge, once, and show it.  A healthy
; read of drive 82h LBA 0 (the stick's MBR) answers status=0, first4=
; 8EC033FA (bytes FA 33 C0 8E) and sig510=0000AA55 - proof the controller
; and the enumerated device survived the park and the bridge read them.
pd_usbhdr       db      13, 10, 'PMDOS.386: USB sector0 read status=', 0
pd_usbs0        db      ' first4=', 0
pd_usbsig       db      ' sig510=', 0
; BC2: each block op RMDOS asks of the bridge (PDAPI_BLOCK), so a DIR U:
; in a DOS box shows the stick's sectors coming through the bridge
pd_blkhdr       db      13, 10, 'PMDOS.386: USB blk drv=', 0
pd_blklba       db      ' lba=', 0
pd_blkst        db      ' st=', 0
pd_blkd0        db      ' d0=', 0
; Stage C: the socket API path's trace (WINSOCK.DLL over the bridge) -
; one line per completed socket op, and a receive shows its first bytes
; as text so a fetch shows "HTTP/..." in the headless log.
pd_skhdr        db      13, 10, 'PMDOS.386: sock fn=', 0
pd_skans        db      ' ans=', 0
pd_skdat        db      ' data=', 0

; BA2 self-test: the SMB1 peer's address and credentials (the step-6
; reference peer smb1peer.pl, reached through the rig's guest-forward at
; 10.0.2.200:139), and the traces.  Resident, because the async tick runs
; the test and the tick's code and data outlive Device_Init.
pd_b2srv        db      '10.0.2.200', 0
pd_b2shr        db      'PMSHARE', 0
pd_b2usr        db      'pmdostest', 0
pd_b2pwd        db      'PmdosSmb!2026', 0
pd_b2pat        db      '*', 0
pd_b2mnt        db      13, 10, 'PMDOS.386: BA2 mount begin (SMB1 \\10.0.2.200\PMSHARE), pumps=', 0
pd_b2mok        db      13, 10, 'PMDOS.386: BA2 mounted, pumps=', 0
pd_b2mfail      db      13, 10, 'PMDOS.386: BA2 mount failed, status=', 0
pd_b2fbeg       db      13, 10, 'PMDOS.386: BA2 findfirst begin, pumps=', 0
pd_b2fok        db      13, 10, 'PMDOS.386: BA2 findfirst done, name=', 0
pd_b2fattr      db      ' attr=', 0
pd_b2fsize      db      ' size=', 0
pd_b2ffail      db      13, 10, 'PMDOS.386: BA2 findfirst failed, status=', 0

; BA2b: the V86 API redirector path's trace (one line per finished op).
pd_rdop         db      13, 10, 'PMDOS.386: BA2 API redir op=', 0
pd_rdst         db      ' status=', 0
pd_rdnm         db      ' name=', 0
pd_rdat         db      ' attr=', 0
pd_rdsz         db      ' size=', 0
pd_rdres        db      ' result=', 0

;---------------------------------------------------------------------
; Stage D - the NDIS MAC face.  The characteristics tables handed to the
; NDIS wrapper at registration, the MAC's names, the handle/context NDIS
; returns, the kernel card's MAC (from NIC_INFO), the one binding, and the
; OID answer table the Request handler walks.  Data values mirror the
; in-box NE2000.386 so a known-good MAC's replies are matched.
;---------------------------------------------------------------------
; The MAC-characteristics name.  NDIS's svc 33h records it as the NDIS_STRING
; at chartable+2Eh and hands it back as the AddAdapter name.  ndis.386's
; protocol manager matches the PROTOCOL.INI logical card name to this: the
; 'ms$' prefix is stripped from the netcard= logical ('ms$pmdos' -> 'pmdos')
; and compared to the MAC name, so the MAC name is the bare module name with
; NO '$' - the '$' belongs on the card section's DriverName, not here (the
; in-box NE2000.386 registers 'Ne2000', not 'MS2000$').  The trailing NUL is
; inside the length, as NE2000 does (len counts the NUL).
pd_macname      db      'PMDOS', 0               ; MAC name = module name, no '$'
PD_MACNAME_LEN  EQU     $ - pd_macname           ; = 6, the NUL included (NE2000's convention)
pd_macregname   db      'PMDOS'                  ; MAC-registration name
PD_MACREGNAME_LEN EQU   $ - pd_macregname

; The MAC-registration (svc 31h) descriptor.  NDIS sizes an allocation
; from the first two words, copies the block and returns it as the MAC
; handle; it does not interpret the three identity words, so NE2000's
; proven values are kept (their individual meaning is not needed here).
                ALIGN   2
pd_reg_desc     dw      0, 3                     ; sizing words (-> 5-word block)
                dw      67B0h, 611Fh, 7154h      ; MAC-level identity (from NE2000)
                dw      PD_MACREGNAME_LEN, PD_MACREGNAME_LEN
                dd      OFFSET FLAT:pd_macregname

; The adapter characteristics table (svc 33h), 36h bytes: version, reserved,
; ten upper-edge handler pointers, then the name as an NDIS_STRING.
                ALIGN   4
pd_macchar      db      3, 0                     ; +00 major, +01 minor
                dd      0                        ; +02 reserved (dword)
                dd      OFFSET FLAT:mac_open     ; +06
                dd      OFFSET FLAT:mac_close    ; +0A
                dd      OFFSET FLAT:mac_send     ; +0E
                dd      OFFSET FLAT:mac_xfer     ; +12
                dd      OFFSET FLAT:mac_reset    ; +16
                dd      OFFSET FLAT:mac_request  ; +1A
                dd      OFFSET FLAT:mac_qstats   ; +1E
                dd      OFFSET FLAT:mac_unload   ; +22
                dd      OFFSET FLAT:mac_addadap  ; +26
                dd      OFFSET FLAT:mac_remadap  ; +2A
                dw      PD_MACNAME_LEN, PD_MACNAME_LEN
                dd      OFFSET FLAT:pd_macname

; MAC runtime state.
pd_macaddr      db      6 dup(0)                 ; the kernel NIC's MAC (NIC_INFO)
                db      0, 0
pd_ndis_ok      db      0                        ; 1 once NDIS accepted the adapter
                db      0, 0, 0
pd_ndis_mach    dd      0                        ; svc 31h: the MAC handle
pd_ndis_ctx     dd      0                        ; svc 33h: NDIS's adapter context (out)
pd_ndis_status  dd      0                        ; svc 33h: the out status cell
                ALIGN   4
pd_adapter      dd      0, 0, 0, 0               ; our MacAdapterContext (svc 33h arg4)
pd_ndis_adapterobj dd   0                        ; svc 35h: the bindable adapter object (out)
pd_ndis_aastatus dd     0                        ; svc 35h: saved status (trace + AddAdapter result)
pd_ndis_cfgh    dd      0                        ; svc 05h: the opened configuration handle (out)
pd_ndis_cfgstatus dd    0                        ; svc 05h: the open status (out)

; The one binding (NetBEUI): OpenAdapter fills it and returns its address
; as the MacBindingHandle.
pd_bind_used    dd      0                        ; 0 free / 1 open
pd_bind_nbc     dd      0                        ; the NdisBindingContext NDIS gave
pd_bind_adap    dd      0                        ; the MacAdapterContext NDIS gave

; D1c receive: the frame being indicated lives in the low page at PGO_RXBUF.
pd_rx_status    dd      0                        ; svc 3Dh arg1: protocol's return
pd_rx_len       dd      0                        ; the current RX frame's whole length
pd_rx_cnt       dd      0                        ; frames taken this drain (a cap guard)
pd_rx_any       db      0                        ; 1 if a frame was indicated this drain
pd_rx_busy      db      0                        ; re-entry guard for pd_rx_pump
pd_rx_traced    db      0                        ; trace the first indicate only
pd_rx_xtraced   db      0                        ; trace the first mac_xfer only

; Settable per-adapter OID stores, and the constant OID values.
pd_pktfilter    dd      0                        ; OID_GEN_CURRENT_PACKET_FILTER
pd_lookahead    dd      0EEh                     ; OID_GEN_CURRENT_LOOKAHEAD (238)
pd_v_hwstatus   dd      0                        ; NdisHardwareStatusReady
pd_v_media      dd      0                        ; NdisMedium802_3
pd_v_maxlook    dd      0EEh                     ; 238
pd_v_maxframe   dd      05DCh                    ; 1500
pd_v_linkspeed  dd      0186A0h                  ; 100000 = 10 Mbit (100bps units)
pd_v_bufspace   dd      04000h                   ; 16K tx/rx buffer space
pd_v_blocksize  dd      05EAh                    ; 1514 (one frame per block)
pd_v_drvver     dw      0300h                    ; 3.0
pd_v_maxtotal   dd      05EAh                    ; 1514
pd_v_macopts    dd      0Fh                      ; COPY_LOOKAHEAD|RX_SERIALIZED|
                                                 ;  TRANSFERS_NOT_PEND|NO_LOOPBACK
pd_v_maxlist    dd      16                       ; OID_802_3_MAXIMUM_LIST_SIZE

; OID_GEN_SUPPORTED_LIST payload.
                ALIGN   4
pd_oidlist      dd      OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS
                dd      OID_GEN_MEDIA_SUPPORTED, OID_GEN_MEDIA_IN_USE
                dd      OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE
                dd      OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE
                dd      OID_GEN_RECEIVE_BUFFER_SPACE, OID_GEN_TRANSMIT_BLOCK_SIZE
                dd      OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_DESCRIPTION
                dd      OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD
                dd      OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE
                dd      OID_GEN_MAC_OPTIONS
                dd      OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS
                dd      OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE
PD_OIDLIST_LEN  EQU     $ - pd_oidlist
pd_vendordesc   db      'PM-DOS bridge NDIS MAC', 0
PD_VENDORDESC_LEN EQU   $ - pd_vendordesc

; The Query-OID answer table the Request handler walks: {OID, length,
; source pointer}, terminated by a zero OID.  Lengths and values mirror
; NE2000's.  The pointer sources are filled in two passes - the ones that
; need a run-time address (the value cells, the lists) are constants here.
                ALIGN   4
pd_oidtab       dd      OID_GEN_SUPPORTED_LIST,        PD_OIDLIST_LEN,    OFFSET FLAT:pd_oidlist
                dd      OID_GEN_HARDWARE_STATUS,       4,                 OFFSET FLAT:pd_v_hwstatus
                dd      OID_GEN_MEDIA_SUPPORTED,       4,                 OFFSET FLAT:pd_v_media
                dd      OID_GEN_MEDIA_IN_USE,          4,                 OFFSET FLAT:pd_v_media
                dd      OID_GEN_MAXIMUM_LOOKAHEAD,     4,                 OFFSET FLAT:pd_v_maxlook
                dd      OID_GEN_MAXIMUM_FRAME_SIZE,    4,                 OFFSET FLAT:pd_v_maxframe
                dd      OID_GEN_LINK_SPEED,            4,                 OFFSET FLAT:pd_v_linkspeed
                dd      OID_GEN_TRANSMIT_BUFFER_SPACE, 4,                 OFFSET FLAT:pd_v_bufspace
                dd      OID_GEN_RECEIVE_BUFFER_SPACE,  4,                 OFFSET FLAT:pd_v_bufspace
                dd      OID_GEN_TRANSMIT_BLOCK_SIZE,   4,                 OFFSET FLAT:pd_v_blocksize
                dd      OID_GEN_RECEIVE_BLOCK_SIZE,    4,                 OFFSET FLAT:pd_v_blocksize
                dd      OID_GEN_VENDOR_DESCRIPTION,    PD_VENDORDESC_LEN, OFFSET FLAT:pd_vendordesc
                dd      OID_GEN_CURRENT_PACKET_FILTER, 4,                 OFFSET FLAT:pd_pktfilter
                dd      OID_GEN_CURRENT_LOOKAHEAD,     4,                 OFFSET FLAT:pd_lookahead
                dd      OID_GEN_DRIVER_VERSION,        2,                 OFFSET FLAT:pd_v_drvver
                dd      OID_GEN_MAXIMUM_TOTAL_SIZE,    4,                 OFFSET FLAT:pd_v_maxtotal
                dd      OID_GEN_MAC_OPTIONS,           4,                 OFFSET FLAT:pd_v_macopts
                dd      OID_802_3_PERMANENT_ADDRESS,   6,                 OFFSET FLAT:pd_macaddr
                dd      OID_802_3_CURRENT_ADDRESS,     6,                 OFFSET FLAT:pd_macaddr
                dd      OID_802_3_MAXIMUM_LIST_SIZE,   4,                 OFFSET FLAT:pd_v_maxlist
                dd      OID_802_3_MULTICAST_LIST,      0,                 OFFSET FLAT:pd_macaddr
                dd      0, 0, 0                   ; end marker

pd_msgndis      db      13, 10, 'PMDOS.386: NDIS present, registering MAC', 0
pd_msgnondis    db      13, 10, 'PMDOS.386: no NDIS wrapper - MAC face skipped', 13, 10, 0
pd_msgmach      db      13, 10, 'PMDOS.386: NDIS RegisterMac handle=', 0
pd_msgreg       db      13, 10, 'PMDOS.386: NDIS RegisterAdapter status=', 0
pd_msgregok     db      13, 10, 'PMDOS.386: NDIS MAC registered, ctx=', 0
pd_msgopen      db      13, 10, 'PMDOS.386: NDIS OpenAdapter -> binding up', 0
pd_msgmoc       db      13, 10, 'PMDOS.386: mac_open CALLED by a protocol', 0
pd_msgmrc       db      13, 10, 'PMDOS.386: mac_request CALLED (first)', 0
pd_mr_traced    db      0
pd_ms_traced    db      0                        ; mac_send: trace only the first TX
                db      0, 0
pd_msg_tx       db      13, 10, 'PMDOS.386: mac_send TX frame -> NIC_SEND', 0
pd_msg_rx       db      13, 10, 'PMDOS.386: RX frame -> NetBEUI indicate', 0
pd_msg_xfer     db      13, 10, 'PMDOS.386: mac_xfer copy to NetBEUI', 0
; Stage-D bind diagnostics: the lifecycle handlers ndis.386 drives while it
; sets up a binding.  Which of these fires tells us how far the handshake got.
pd_msg_aa       db      13, 10, 'PMDOS.386: mac_addadap args=', 0
pd_sp           db      ' ', 0
pd_msg_nm       db      ' name@arg3=', 0
pd_msg_aard     db      13, 10, 'PMDOS.386: svc35 NdisRegisterAdapter status=', 0
pd_msg_cfg      db      13, 10, 'PMDOS.386: NdisOpenConfiguration status=', 0
pd_msg_cfgh     db      ' cfgh=', 0
pd_msg_rst      db      13, 10, 'PMDOS.386: mac_reset CALLED', 0
pd_msg_cls      db      13, 10, 'PMDOS.386: mac_close CALLED', 0
pd_msg_qs       db      13, 10, 'PMDOS.386: mac_qstats CALLED', 0
pd_msg_unl      db      13, 10, 'PMDOS.386: mac_unload CALLED', 0
_LDATA  ENDS

;=====================================================================
; LOCKED CODE - resident for the life of the system: the control
; procedure, the V86 API entry, the bridge call and the E9h tracers.
;=====================================================================
_LTEXT  SEGMENT
        ASSUME  ds:FLAT, es:FLAT, ss:FLAT

pd_control:
        cmp     eax,DEVICE_INIT
        je      pc_devinit
        cmp     eax,SYS_CRITICAL_INIT
        je      pd_syscrit
pc_other:
        clc
        ret
pc_devinit:
        jmp     pd_devinit

;---------------------------------------------------------------------
; pd_syscrit - Sys_Critical_Init, the earliest VxD message.  Hook V86
; INT 13h NOW, before WfW's enhanced-mode init first reads the USB BIOS
; drive's sector 0: too late (at Device_Init) and WfW has already read it
; off a BIOS that cannot reach the controller, got zeroes, and wedged on
; the zero-geometry divide.  The mask starts covering the USB BIOS range
; (81h..85h) and Device_Init narrows it to what usbm_owns actually claims;
; until the bridge is up the hook answers those drives "not ready" (a
; clean error, not a zero sector), which is enough to keep WfW off the
; divide.  The stick is still reached as D: through RMDOS, not INT 13h.
;---------------------------------------------------------------------
pd_syscrit:
        pushad
        mov     byte ptr [pd_usbmask],3Eh
        mov     eax,13h
        mov     esi,OFFSET FLAT:pd_i13hook
        VXDCALL S_HOOK_V86_INT_CHAIN
        mov     esi,OFFSET FLAT:pd_msgscrit
        call    pd_e9str
        popad
        clc
        ret

;---------------------------------------------------------------------
; pd_bridge_call - perform one driver call through the trampoline.
;   EAX = NDTF_* flag, EDX = index, EBX = driver function, ECX = length,
;   EDI = the buffer's OFFSET in the low page (PGO_BUF, or PGO_RES for a
;   call whose buffer must persist between calls).  The driver reads and
;   writes that buffer; the low page is 1:1 under both maps.
;   -> EAX = brg_entry's answer, CF = the driver's carry.
;
; Interrupts are held off from before the shared page is touched until
; after the switch returns, so no interrupt-time caller (the async tick,
; a hardware ISR) can interpose on the page mid-call; the caller's own
; interrupt flag is then restored, so this is safe to call from such a
; context.  A re-entrant call (should not happen under that discipline)
; is refused rather than allowed to corrupt the page.  If the bridge did
; not come up, CF is set and EAX = FFFFFFFFh; on a refused re-entry,
; FFFFFFFEh.  Keeps every register but EAX and the flags.
;---------------------------------------------------------------------
pd_bridge_call:
        pushfd                                  ; the caller's flags (its IF)
        cli
        push    ebp
        mov     ebp,[pd_pg]
        test    ebp,ebp
        jz      pbc_notready
        cmp     byte ptr [pd_busy],0
        jne     pbc_reentry
        mov     byte ptr [pd_busy],1
        add     edi,ebp                         ; the buffer, absolute linear
        mov     [ebp+PGO_RQEAX],eax
        mov     [ebp+PGO_RQEDX],edx
        mov     [ebp+PGO_RQEBX],ebx
        mov     [ebp+PGO_RQECX],ecx
        mov     [ebp+PGO_RQEDI],edi
        mov     eax,ebp                         ; the page base = the trampoline
        call    eax                             ; -> EAX answer, flags = driver's,
                                                ; IF still off (the trampoline no
                                                ; longer forces it on)
        mov     byte ptr [pd_busy],0            ; (a mov leaves the driver's CF)
        jc      pbc_drvcf
        pop     ebp
        popfd                                   ; the caller's IF back
        clc
        ret
pbc_drvcf:
        pop     ebp
        popfd
        stc                                     ; the driver raised CF
        ret
pbc_reentry:
        pop     ebp
        popfd
        mov     eax,0FFFFFFFEh
        stc
        ret
pbc_notready:
        pop     ebp
        popfd
        mov     eax,0FFFFFFFFh
        stc
        ret

;---------------------------------------------------------------------
; pd_tick - a global time-out every PD_TICKMS, the async heart of the
; bridge.  While Windows owns the machine the parked kernel's network
; has no clock tick and no card interrupt of its own, so this drives it:
; one SOCK_PUMP bridge call takes frames off the card (NIC_SERVICE),
; runs the protocol over them and runs the stack's timers - exactly what
; the kernel's own tick would do, at the same 55 ms cadence.  Windows is
; never paused for it: the stack runs here, in the background, between
; everything else.  The BA1 self-test's name lookup is polled here until
; it is done.  The time-out is one-shot, so this re-arms it.
;
; Entered at interrupt time with EBX = the current VM.  pd_bridge_call
; holds interrupts off across each switch and restores them, so firing
; here cannot collide with a DOS box's API call that is mid-bridge.
;---------------------------------------------------------------------
pd_tick:
        pushad
        ; one step of the parked stack - takes frames off the card, runs
        ; the protocol and the timers (DHCP among them).  The buffer is
        ; the tick's own scratch, never PGO_BUF, so a DOS box's API call
        ; cannot have its result overwritten from here.
        mov     eax,NDTF_STACK
        xor     edx,edx
        mov     ebx,SOCK_PUMP
        xor     ecx,ecx
        mov     edi,PGO_IFS
        call    pd_bridge_call
        inc     dword ptr [pd_pumps]
        call    pd_rx_pump                      ; D1c: hand NetBEUI its frames

        cmp     byte ptr [pd_ba1],1
IFDEF BA2_TICKSELFTEST
        ja      pt_ba2                          ; 2 = DNS done: drive the redirector test
ELSE
        ; BA2b: the redirector is driven by the V86 API (PDAPI_REDIR), which
        ; RMDOS - or the BA2API test program - polls.  There is one worker
        ; fibre and one bounce buffer, so the tick must not also run a
        ; redirector op; it keeps pumping the stack (above) so the API's
        ; op advances between the caller's polls.  pt_ba2 stays assembled
        ; for reproducing BA2a (-d BA2_TICKSELFTEST), but is not reached.
        ja      pt_usb                          ; 2 = DNS done: run the USB self-test once
ENDIF
        je      pt_poll                         ; 1 = lookup outstanding

        ; pd_ba1 = 0: wait for the interface.  DHCP runs on the tick just
        ; driven, so this comes up a little after the first ticks; the
        ; name is already in PGO_RES (Device_Init put it there).
        mov     eax,NDTF_STACK
        xor     edx,edx
        mov     ebx,SOCK_IFSTAT
        mov     ecx,SOCKIF_LEN
        mov     edi,PGO_IFS
        call    pd_bridge_call
        jc      pt_hb
        mov     ebp,[pd_pg]
        cmp     byte ptr [ebp+PGO_IFS+SIF_UP],0
        je      pt_hb                           ; not up yet; wait
        ; up: start the lookup through the parked stack's DNS
        mov     eax,NDTF_STACK
        xor     edx,edx
        mov     ebx,SOCK_RESOLVE
        mov     ecx,SOCKRES_LEN
        mov     edi,PGO_RES
        call    pd_bridge_call
        mov     byte ptr [pd_ba1],1
        mov     esi,OFFSET FLAT:pd_tifup
        call    pd_e9str
        mov     eax,[pd_pumps]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pt_hb

pt_poll:
        ; poll the outstanding lookup
        mov     eax,NDTF_STACK
        xor     edx,edx
        mov     ebx,SOCK_RESOLVE
        mov     ecx,SOCKRES_LEN
        mov     edi,PGO_RES
        call    pd_bridge_call
        jc      pt_hb                           ; busy this tick: try the next
        cmp     eax,NET_PENDING
        je      pt_hb
        mov     byte ptr [pd_ba1],2             ; DONE (with an address) or FAIL
        mov     esi,OFFSET FLAT:pd_tdone
        call    pd_e9str
        mov     eax,[pd_pumps]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_tstate
        call    pd_e9str
        mov     ebp,[pd_pg]
        mov     eax,[ebp+PGO_RES+SR_STATE]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_tip
        call    pd_e9str
        mov     ebp,[pd_pg]
        mov     eax,[ebp+PGO_RES+SR_IP]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pt_hb                           ; this tick's work is done

;---------------------------------------------------------------------
; BA2 - with the stack up and the DNS self-test done, prove the
; redirector runs through the bridge under the async model: a resumable
; SMB1 MOUNT then FINDFIRST against the reference peer, one slice a tick,
; the parked stack advancing between slices on this tick and the card's
; receive interrupt.  pd_ba2 walks 0 prepare the mount, 1 mount, 2
; prepare the find, 3 find, 4 done.  The request block lives in the
; bounce buffer (NR_SIZE <= PGO_RES - PGO_BUF); the driver keeps its own
; copy across slices and writes REDIR_PENDING here until the op finishes.
;---------------------------------------------------------------------
pt_ba2:
        movzx   eax,byte ptr [pd_ba2]
        cmp     eax,4
        jae     pt_hb                           ; done
        or      eax,eax
        jz      pt_b2prep
        cmp     eax,1
        je      pt_b2mount
        cmp     eax,2
        je      pt_b2fprep
        jmp     pt_b2find                       ; 3

pt_b2prep:
        mov     ebp,[pd_pg]
        lea     edi,[ebp+PGO_BUF]
        mov     ecx,NR_SIZE
        call    pd_rzero
        mov     ebp,[pd_pg]
        mov     dword ptr [ebp+PGO_BUF+NR_DRIVE],PD_SELFDRIVE
        mov     dword ptr [ebp+PGO_BUF+NR_SPARE],1      ; SMB1 (NR_SPARE picks it)
        lea     edi,[ebp+PGO_BUF+NR_SERVER]
        mov     esi,OFFSET FLAT:pd_b2srv
        call    pd_rcopy
        mov     ebp,[pd_pg]
        lea     edi,[ebp+PGO_BUF+NR_SHARE]
        mov     esi,OFFSET FLAT:pd_b2shr
        call    pd_rcopy
        mov     ebp,[pd_pg]
        lea     edi,[ebp+PGO_BUF+NR_USER]
        mov     esi,OFFSET FLAT:pd_b2usr
        call    pd_rcopy
        mov     ebp,[pd_pg]
        lea     edi,[ebp+PGO_BUF+NR_PASS]
        mov     esi,OFFSET FLAT:pd_b2pwd
        call    pd_rcopy
        mov     byte ptr [pd_ba2],1
        mov     esi,OFFSET FLAT:pd_b2mnt
        call    pd_e9str
        mov     eax,[pd_pumps]
        call    pd_e9hex
        jmp     pt_hb

pt_b2mount:
        mov     eax,NDTF_REDIR
        xor     edx,edx
        mov     ebx,REDIR_MOUNT OR REDIR_RESUME
        mov     ecx,NR_SIZE
        mov     edi,PGO_BUF
        call    pd_bridge_call
        jc      pt_hb                           ; bridge busy this tick; poll next
        mov     ebp,[pd_pg]
        mov     eax,[ebp+PGO_BUF+NR_STATUS]
        cmp     eax,REDIR_PENDING
        je      pt_hb                           ; still working
        or      eax,eax
        jnz     pt_b2mfail
        mov     byte ptr [pd_ba2],2             ; mounted
        mov     esi,OFFSET FLAT:pd_b2mok
        call    pd_e9str
        mov     eax,[pd_pumps]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pt_hb
pt_b2mfail:
        mov     byte ptr [pd_ba2],4
        push    eax
        mov     esi,OFFSET FLAT:pd_b2mfail
        call    pd_e9str
        pop     eax
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pt_hb

pt_b2fprep:
        mov     ebp,[pd_pg]
        lea     edi,[ebp+PGO_BUF]
        mov     ecx,NR_SIZE
        call    pd_rzero
        mov     ebp,[pd_pg]
        mov     dword ptr [ebp+PGO_BUF+NR_DRIVE],PD_SELFDRIVE
        mov     dword ptr [ebp+PGO_BUF+NR_ATTR],REDIR_SRCH
        lea     edi,[ebp+PGO_BUF+NR_PATH]
        mov     esi,OFFSET FLAT:pd_b2pat         ; "*" - the share's root, every name
        call    pd_rcopy
        mov     byte ptr [pd_ba2],3
        mov     esi,OFFSET FLAT:pd_b2fbeg
        call    pd_e9str
        mov     eax,[pd_pumps]
        call    pd_e9hex
        jmp     pt_hb

pt_b2find:
        mov     eax,NDTF_REDIR
        xor     edx,edx
        mov     ebx,REDIR_FINDFIRST OR REDIR_RESUME
        mov     ecx,NR_SIZE
        mov     edi,PGO_BUF
        call    pd_bridge_call
        jc      pt_hb
        mov     ebp,[pd_pg]
        mov     eax,[ebp+PGO_BUF+NR_STATUS]
        cmp     eax,REDIR_PENDING
        je      pt_hb
        or      eax,eax
        jnz     pt_b2ffail
        mov     byte ptr [pd_ba2],4             ; found: trace the first entry
        mov     esi,OFFSET FLAT:pd_b2fok
        call    pd_e9str
        mov     ebp,[pd_pg]
        lea     esi,[ebp+PGO_BUF+NR_NAME]        ; the 8.3 name, ASCIIZ
        call    pd_e9str
        mov     esi,OFFSET FLAT:pd_b2fattr
        call    pd_e9str
        mov     ebp,[pd_pg]
        movzx   eax,byte ptr [ebp+PGO_BUF+NR_ATTR]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_b2fsize
        call    pd_e9str
        mov     ebp,[pd_pg]
        mov     eax,[ebp+PGO_BUF+NR_SIZELO]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pt_hb
pt_b2ffail:
        mov     byte ptr [pd_ba2],4
        push    eax
        mov     esi,OFFSET FLAT:pd_b2ffail
        call    pd_e9str
        pop     eax
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pt_hb

;---------------------------------------------------------------------
; pt_usb - the USB-over-bridge self-test, run ONCE after the network
; settled.  Read sector 0 of the parked kernel's USB stick (block drive
; 82h) through the bridge - EAX = NDTF_BLOCK routes brg_entry straight to
; blk_bridge, which masks the PICs and calls the kernel's own block layer.
; A healthy read answers status=0, first4=8EC033FA (the MBR's FA 33 C0 8E)
; and sig510=0000AA55, which proves the controller and the enumerated
; device survived the park and the bridge read real sectors off the stick.
;---------------------------------------------------------------------
pt_usb:
        cmp     byte ptr [pd_usbtest],0
        jne     pt_hb                           ; once only
        mov     byte ptr [pd_usbtest],1
        mov     ebp,[pd_pg]
        lea     edi,[ebp+PGO_BUF]
        mov     ecx,BR_SIZE
        call    pd_rzero                        ; BR_OP, BR_LBA = 0: read sector 0
        mov     ebp,[pd_pg]
        mov     dword ptr [ebp+PGO_BUF+BR_DRIVE],PD_USBDRIVE
        mov     dword ptr [ebp+PGO_BUF+BR_COUNT],1
        mov     eax,NDTF_BLOCK
        xor     edx,edx
        xor     ebx,ebx
        mov     ecx,BR_SIZE
        mov     edi,PGO_BUF
        call    pd_bridge_call
        mov     esi,OFFSET FLAT:pd_usbhdr
        call    pd_e9str
        mov     ebp,[pd_pg]
        mov     eax,[ebp+PGO_BUF+BR_STATUS]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_usbs0
        call    pd_e9str
        mov     ebp,[pd_pg]
        mov     eax,[ebp+PGO_BUF+BR_DATA]       ; the first four bytes of the sector
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_usbsig
        call    pd_e9str
        mov     ebp,[pd_pg]
        movzx   eax,word ptr [ebp+PGO_BUF+BR_DATA+510]  ; the AA55 boot signature
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pt_hb

pt_hb:
        ; a heartbeat every PD_HEARTBEAT ticks: a run shows the tick is
        ; firing and the pump count climbing even before the lookup ends
        inc     dword ptr [pd_hbctr]
        cmp     dword ptr [pd_hbctr],PD_HEARTBEAT
        jb      pt_rearm
        mov     dword ptr [pd_hbctr],0
        mov     esi,OFFSET FLAT:pd_thb
        call    pd_e9str
        mov     eax,[pd_pumps]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_tints         ; the card-interrupt count too:
        call    pd_e9str                         ; a climbing count is the proof
        mov     eax,[pd_ints]                    ; the VPICD receive line is live
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str

pt_rearm:
        mov     eax,PD_TICKMS
        xor     edx,edx
        mov     esi,OFFSET FLAT:pd_tick
        VXDCALL S_SET_GLOBAL_TIME_OUT
        popad
        ret

;---------------------------------------------------------------------
; pd_nicint - VPICD_Hw_Int_Proc for the card's receive line (BA1b).
; Entered at interrupt time, interrupts off, EAX = the IRQ handle, EBX =
; the current VM.  One SOCK_PUMP bridge call takes the frames the card
; raised the interrupt for off its ring, runs the protocol over them and
; runs the stack's timers - the same step the tick makes, but now the
; instant a frame lands, not up to a tick later.  Draining the card
; dismisses the chip's interrupt status (card_service writes it back), so
; a level-triggered line has deasserted by the time this ends it at the
; controller with Phys_EOI; no storm.  The current VM may be any VM, but
; the trampoline's page and the kernel's identity map are global, so the
; switch is good from any of them (as the tick, entered the same way, is).
;
; The hw-int proc and the tick can never be mid-bridge at once:
; pd_bridge_call holds interrupts off across the whole switch, so a
; hardware interrupt cannot land while a bridge call is in progress.
;---------------------------------------------------------------------
pd_nicint:
        pushad
        inc     dword ptr [pd_ints]
        mov     eax,NDTF_STACK
        xor     edx,edx
        mov     ebx,SOCK_PUMP
        xor     ecx,ecx
        mov     edi,PGO_IFS
        call    pd_bridge_call
        call    pd_rx_pump                      ; D1c: hand NetBEUI its frames
        mov     eax,[pd_nich]
        VXDCALL S_VPICD_PHYS_EOI
        popad
        ret

;---------------------------------------------------------------------
; pd_niceoi - VPICD_EOI_Proc, entered EAX = the handle.  The card's
; interrupt is serviced whole in pd_nicint and never reflected on to a
; VM, so this is not reached; it mirrors LRGDISK's EOI proc for a
; well-formed descriptor.
;---------------------------------------------------------------------
pd_niceoi:
        push    eax
        VXDCALL S_VPICD_CLEAR_INT_REQ
        pop     eax
        VXDCALL S_VPICD_PHYS_EOI
        ret

;---------------------------------------------------------------------
; pd_v86api - the V86 API entry (DDB_V86_API_Proc).  Entered at ring 0
; with EBP -> the calling VM's Client_Reg_Struc and EBX = its VM handle,
; DS = ES = the VMM flat data selector (base 0).  The caller reached it
; by far-calling the address INT 2Fh AX=1684h BX=504Dh handed back.
;
; The calling VM is current, so its first megabyte is mapped 1:1 at
; linear 0: the caller's buffer at ES:DX is reachable here as the flat
; linear (ES<<4)+DX, the same 1:1 low memory the bridge already uses.
;
; B1 (AX = PDAPI_NICINFO): do the NIC_INFO bridge call and copy the
; fixed 52 bytes into the caller's buffer.  The copy length is the
; driver's own constant, never a value from the caller, so a caller
; cannot make it write past its buffer's size (it owns where it points).
;---------------------------------------------------------------------
pd_v86api:
        pushad                                  ; pushad leaves EBP = the struct
        movzx   eax,word ptr [ebp+C_EAX]        ; the sub-function
        cmp     ax,PDAPI_NICINFO
        je      pv_nicinfo
        cmp     ax,PDAPI_REDIR
        je      pv_redir
        cmp     ax,PDAPI_BLOCK
        je      pv_block
        cmp     ax,PDAPI_SOCK
        je      pv_sock
        jmp     pv_fail

pv_nicinfo:
        mov     esi,OFFSET FLAT:pd_rv86         ; B1's trace (one call, so no flood)
        call    pd_e9str
        ; the caller's buffer, as a flat linear address
        movzx   edi,word ptr [ebp+C_ES]
        shl     edi,4
        movzx   eax,word ptr [ebp+C_EDX]
        add     edi,eax
        mov     eax,edi
        add     eax,NICI_LEN
        cmp     eax,PD_V86TOP
        ja      pv_fail                         ; runs past the VM's first megabyte
        mov     [pd_clbuf],edi

        mov     eax,NDTF_NIC
        xor     edx,edx
        mov     ebx,NIC_INFO
        mov     ecx,NICI_LEN
        mov     edi,PGO_BUF
        call    pd_bridge_call                  ; -> EAX answer, CF driver CF
        jc      pv_fail

        ; the parked kernel's NIC driver filled the bounce buffer; hand
        ; the fixed 52 bytes to the caller
        push    eax
        mov     esi,[pd_pg]
        add     esi,PGO_BUF
        mov     edi,[pd_clbuf]
        mov     ecx,NICI_LEN
        cld
        rep     movsb
        pop     eax

        mov     word ptr [ebp+C_EAX],ax         ; caller's AX = the answer
        and     word ptr [ebp+C_EFLAGS],0FFFEh  ; caller's CF = 0: it worked

        mov     esi,OFFSET FLAT:pd_rok
        call    pd_e9str
        push    eax
        call    pd_e9hex
        pop     eax
        mov     esi,OFFSET FLAT:pd_rmac
        call    pd_e9str
        mov     esi,[pd_pg]
        add     esi,PGO_BUF + NICI_MAC
        mov     ecx,6
        call    pd_e9mac
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pv_done

;---------------------------------------------------------------------
; pv_redir (PDAPI_REDIR) - run one resumable slice of the caller's
; redirector request through the bridge.  The caller's redir_req is at
; ES:DX (692 bytes); its NR_OP holds the REDIR_* op.  One slice: copy
; the block into the bounce buffer, bridge {NDTF_REDIR, op|REDIR_RESUME}
; into the parked SMBFS driver (which runs it on its worker fibre and
; yields when a socket wait has no data), copy the block back.  The
; caller reads NR_STATUS (and gets it in AX): REDIR_PENDING means poll
; again - the caller yields (INT 2Fh 1680h) so Windows and the async
; tick run and advance the parked stack before the next slice.  CF is
; left clear whenever the slice ran (the status carries the result);
; the copy length is the struct's fixed size, never the caller's, so a
; caller cannot make this write past the buffer it owns.
;---------------------------------------------------------------------
pv_redir:
        ; the caller's redir_req, as a flat linear address
        movzx   edi,word ptr [ebp+C_ES]
        shl     edi,4
        movzx   eax,word ptr [ebp+C_EDX]
        add     edi,eax
        mov     eax,edi
        add     eax,NR_SIZE
        cmp     eax,PD_V86TOP
        ja      pv_fail                         ; runs past the VM's first megabyte
        mov     [pd_clbuf],edi
        cmp     dword ptr [pd_pg],0
        je      pv_rbusy                        ; bridge not up: no page to copy into
        ; copy it into the bounce buffer
        mov     esi,[pd_clbuf]
        mov     edi,[pd_pg]
        add     edi,PGO_BUF
        mov     ecx,NR_SIZE
        cld
        rep     movsb
        ; the driver reads every string field as ASCIIZ, so end each one
        ; in the copy, whatever the caller left in its last byte
        mov     esi,[pd_pg]
        mov     byte ptr [esi+PGO_BUF+NR_PATH+259],0
        mov     byte ptr [esi+PGO_BUF+NR_PATTERN+15],0
        mov     byte ptr [esi+PGO_BUF+NR_NAME+15],0
        mov     byte ptr [esi+PGO_BUF+NR_SERVER+63],0
        mov     byte ptr [esi+PGO_BUF+NR_SHARE+79],0
        mov     byte ptr [esi+PGO_BUF+NR_USER+63],0
        mov     byte ptr [esi+PGO_BUF+NR_PASS+63],0
        mov     byte ptr [esi+PGO_BUF+NR_DOMAIN+63],0
        ; a read or write moves NR_COUNT bytes at NR_BUFPTR, a flat linear
        ; address: that span must lie inside the VM's first megabyte
        mov     eax,[esi+PGO_BUF+NR_OP]
        and     eax,NOT REDIR_RESUME            ; the op the driver will run
        cmp     eax,REDIR_READ
        je      pvr_chkbuf
        cmp     eax,REDIR_WRITE
        jne     pvr_bufok
pvr_chkbuf:
        mov     ecx,[esi+PGO_BUF+NR_COUNT]
        cmp     ecx,10000h                      ; a V86 buffer is one segment at most
        ja      pvr_badbuf
        mov     eax,[esi+PGO_BUF+NR_BUFPTR]
        add     eax,ecx
        jc      pvr_badbuf
        cmp     eax,PD_V86TOP
        jbe     pvr_bufok
pvr_badbuf:
        mov     word ptr [ebp+C_EAX],5          ; access denied
        jmp     pv_fail
pvr_bufok:
        ; one slice; the op comes from NR_OP, ORed with REDIR_RESUME
        mov     ebx,[esi+PGO_BUF+NR_OP]
        or      ebx,REDIR_RESUME
        mov     eax,NDTF_REDIR
        xor     edx,edx
        mov     ecx,NR_SIZE
        mov     edi,PGO_BUF
        call    pd_bridge_call                  ; preserves EBP (the client struct)
        jc      pv_rbusy
        ; copy the request block back to the caller
        mov     esi,[pd_pg]
        add     esi,PGO_BUF
        mov     edi,[pd_clbuf]
        mov     ecx,NR_SIZE
        cld
        rep     movsb
        ; the caller's AX = NR_STATUS, carry clear
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+NR_STATUS]
        mov     word ptr [ebp+C_EAX],ax
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        ; trace only a finished op, so polling does not flood the log
        cmp     eax,REDIR_PENDING
        je      pv_done
        call    pd_redir_trace
        jmp     pv_done
pv_rbusy:
        ; the tick held the bridge this instant: tell the caller to poll
        ; again (REDIR_PENDING), carry clear
        mov     word ptr [ebp+C_EAX],REDIR_PENDING
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        jmp     pv_done

;---------------------------------------------------------------------
; pv_block (PDAPI_BLOCK) - one USB-disk sector read or write for RMDOS,
; which cannot reach the kernel's block layer except through here.  The
; caller's block_req is at ES:DX (BR_SIZE); BR_OP/BR_DRIVE/BR_LBA/BR_COUNT
; say what, BR_DATA carries the sector.  Unlike the redirector this is not
; resumable: blk_bridge masks the PICs and polls the USB transfer to the
; end inside the one bridge window, so one call finishes it.  The copy
; length is the struct's fixed size, never the caller's.
;---------------------------------------------------------------------
pv_block:
        movzx   edi,word ptr [ebp+C_ES]
        shl     edi,4
        movzx   eax,word ptr [ebp+C_EDX]
        add     edi,eax
        mov     eax,edi
        add     eax,BR_SIZE
        cmp     eax,PD_V86TOP
        ja      pv_fail                         ; runs past the VM's first megabyte
        mov     [pd_clbuf],edi
        cmp     dword ptr [pd_pg],0
        je      pv_bbusy                        ; bridge not up: no page to copy into
        mov     esi,[pd_clbuf]
        mov     edi,[pd_pg]
        add     edi,PGO_BUF
        mov     ecx,BR_SIZE
        cld
        rep     movsb
        mov     esi,[pd_pg]
        ; only a read or a write of the one sector BR_DATA has room for
        cmp     dword ptr [esi+PGO_BUF+BR_OP],1
        ja      pv_fail
        cmp     dword ptr [esi+PGO_BUF+BR_COUNT],1
        jne     pv_fail
        mov     ebx,[esi+PGO_BUF+BR_OP]
        mov     eax,NDTF_BLOCK
        xor     edx,edx
        mov     ecx,BR_SIZE
        mov     edi,PGO_BUF
        call    pd_bridge_call                  ; -> brg_entry -> blk_bridge
        jc      pv_bbusy
        mov     esi,[pd_pg]
        add     esi,PGO_BUF
        mov     edi,[pd_clbuf]
        mov     ecx,BR_SIZE
        cld
        rep     movsb                           ; the data and status back
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+BR_STATUS]
        mov     word ptr [ebp+C_EAX],ax         ; caller's AX = BR_STATUS, CF clear
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        ; trace drive / LBA / status, so a DIR U: shows RMDOS reading the
        ; stick through the bridge
        mov     esi,OFFSET FLAT:pd_blkhdr
        call    pd_e9str
        mov     esi,[pd_pg]
        movzx   eax,byte ptr [esi+PGO_BUF+BR_DRIVE]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_blklba
        call    pd_e9str
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+BR_LBA]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_blkst
        call    pd_e9str
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+BR_STATUS]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_blkd0
        call    pd_e9str
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+BR_DATA]       ; the first four bytes read
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        jmp     pv_done
pv_bbusy:
        ; the tick held the bridge this instant: AX=0FFFFh, CF set = retry
        mov     word ptr [ebp+C_EAX],0FFFFh
        or      word ptr [ebp+C_EFLAGS],1
        jmp     pv_done

;---------------------------------------------------------------------
; pv_sock (PDAPI_SOCK) - one socket op on the parked kernel's TCP/IP
; stack for WINSOCK.DLL.  The caller's request block is at ES:DX, BX =
; the SOCK_* function, CX = the request length.  Copy it into the
; bounce buffer, bridge {NDTF_STACK, EBX=fn, ECX=len, EDI=PGO_BUF} into
; the parked stack (every socket op is instant - the stack never
; blocks), copy the (possibly updated) block back, and hand the caller
; EAX = the answer with CF = the stack's carry.  The length is bounded
; to the bounce buffer's room, so a caller cannot make the copy overrun.
;---------------------------------------------------------------------
pv_sock:
        movzx   ecx,word ptr [ebp+C_ECX]        ; caller CX = the request length
        or      ecx,ecx
        jz      pv_fail                         ; a zero-length request is a bad call
        cmp     ecx,PDS_MAXLEN
        ja      pv_fail                         ; too big for the bounce buffer
        mov     [pd_socklen],ecx
        movzx   ebx,word ptr [ebp+C_EBX]        ; caller BX = the SOCK_* function
        mov     [pd_sockfn],ebx
        ; a V86 caller's buffer at ES:DX, as a flat linear address
        movzx   edi,word ptr [ebp+C_ES]
        shl     edi,4
        movzx   eax,word ptr [ebp+C_EDX]
        add     edi,eax
        mov     eax,edi
        add     eax,ecx
        cmp     eax,PD_V86TOP
        ja      pv_fail                         ; runs past the VM's first megabyte
        mov     [pd_clbuf],edi
        call    pd_sockop                       ; copy in, bridge, copy back, answer
        jmp     pv_done

;---------------------------------------------------------------------
; pd_pmapi - the PM API entry (DDB_PM_API_Proc), for WINSOCK.DLL and
; any other 16-bit PROTECTED-mode program under enhanced mode.  Reached
; the same way (INT 2Fh AX=1684h, BX=504Dh hands back the call-in
; address - a PM caller gets this proc, a V86 caller gets pd_v86api),
; with EBP -> the caller's Client_Reg_Struc.
;
; A PM caller's ES:DX would be a SELECTOR:offset, which is not a linear
; address, so the PM socket call passes its request buffer a different
; way: EDX = the buffer's flat LINEAR address, which the DLL gets by
; allocating the buffer in conventional memory (KERNEL GlobalDosAlloc:
; its paragraph address << 4 is 1:1 linear in the System VM, the same
; low memory the bridge already uses).  BX = the SOCK_* function, CX =
; the length.  Only PDAPI_SOCK is served here (the redirector and block
; APIs are for RMDOS, which is a V86 caller).
;---------------------------------------------------------------------
pd_pmapi:
        pushad                                  ; pushad leaves EBP = the struct
        movzx   eax,word ptr [ebp+C_EAX]
        cmp     ax,PDAPI_SOCK
        jne     pp_fail
        movzx   ecx,word ptr [ebp+C_ECX]
        or      ecx,ecx
        jz      pp_fail
        cmp     ecx,PDS_MAXLEN
        ja      pp_fail
        mov     [pd_socklen],ecx
        movzx   ebx,word ptr [ebp+C_EBX]
        mov     [pd_sockfn],ebx
        mov     edx,[ebp+C_EDX]                 ; PM: EDX = the buffer's flat linear
        mov     eax,edx
        add     eax,ecx
        jc      pp_fail
        cmp     eax,PD_V86TOP
        ja      pp_fail                         ; not conventional memory
        mov     [pd_clbuf],edx
        call    pd_sockop
        popad
        ret
pp_fail:
        or      word ptr [ebp+C_EFLAGS],1       ; caller's CF = 1: it did not
        popad
        ret

;---------------------------------------------------------------------
; pd_sockop - the shared socket-op core for pv_sock (V86) and pd_pmapi
; (PM).  Assumes pd_clbuf = the caller buffer's flat linear address,
; pd_socklen = its length, pd_sockfn = the SOCK_* function, EBP = the
; client struct.  Copies the request into the bounce buffer, bridges
; one socket op {NDTF_STACK, EBX=fn} into the parked stack, copies the
; (possibly updated) block back, and sets the caller's EAX and CF.
; The length is already bounded to the bounce buffer's room.
;---------------------------------------------------------------------
pd_sockop:
        cmp     dword ptr [pd_pg],0
        je      pso_busy                        ; bridge not up: no page to copy into
        mov     esi,[pd_clbuf]
        mov     edi,[pd_pg]
        add     edi,PGO_BUF
        mov     ecx,[pd_socklen]
        cld
        rep     movsb
        mov     eax,NDTF_STACK
        xor     edx,edx
        mov     ebx,[pd_sockfn]
        mov     ecx,[pd_socklen]
        mov     edi,PGO_BUF
        call    pd_bridge_call                  ; -> EAX answer, CF = the stack's carry
        jnc     pso_ok
        cmp     eax,0FFFF0000h
        jae     pso_busy                        ; bridge busy / not ready: tell caller to retry
        ; a socket error: AX = the error, carry set
        mov     word ptr [ebp+C_EAX],ax
        or      word ptr [ebp+C_EFLAGS],1
        ret
pso_ok:
        mov     [pd_sockans],eax
        mov     esi,[pd_pg]
        add     esi,PGO_BUF
        mov     edi,[pd_clbuf]
        mov     ecx,[pd_socklen]
        cld
        rep     movsb
        mov     eax,[pd_sockans]
        mov     dword ptr [ebp+C_EAX],eax
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        call    pd_sock_trace
        ret
pso_busy:
        mov     word ptr [ebp+C_EAX],0FFFFh
        or      word ptr [ebp+C_EFLAGS],1
        ret

;---------------------------------------------------------------------
; pd_sock_trace - E9 one completed socket op: the function and the
; answer, and for a receive that returned data, the first bytes of it
; as text (so a fetch shows "HTTP/..." in the headless log).  The
; heavily polled ops - SOCK_STATE, and a SOCK_RECV that got nothing -
; are skipped so polling does not flood the log.  Preserves every
; register (so EBP, the client struct, is untouched).
;---------------------------------------------------------------------
pd_sock_trace:
        pushad
        mov     eax,[pd_sockfn]
        cmp     eax,SOCK_STATE
        je      pst_done                        ; polled heavily: do not trace
        cmp     eax,SOCK_RECV
        jne     pst_show
        cmp     dword ptr [pd_sockans],0
        je      pst_done                        ; a receive with no data yet: skip
pst_show:
        mov     esi,OFFSET FLAT:pd_skhdr
        call    pd_e9str
        mov     eax,[pd_sockfn]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_skans
        call    pd_e9str
        mov     eax,[pd_sockans]
        call    pd_e9hex
        cmp     dword ptr [pd_sockfn],SOCK_RECV
        jne     pst_crlf
        ; a receive: show up to 24 bytes of the data, as text
        mov     esi,OFFSET FLAT:pd_skdat
        call    pd_e9str
        mov     ecx,[pd_sockans]
        cmp     ecx,24
        jbe     pst_len
        mov     ecx,24
pst_len:
        mov     esi,[pd_pg]
        add     esi,PGO_BUF + PDS_IOHDR         ; the data after the SOCK_IO header
        call    pd_e9raw
pst_crlf:
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
pst_done:
        popad
        ret

;---------------------------------------------------------------------
; pd_e9raw - write ECX bytes at DS:ESI to port E9h as text; a control
; byte is shown as '.'.  Preserves every register.
;---------------------------------------------------------------------
pd_e9raw:
        push    eax
        push    ecx
        push    edx
        push    esi
        mov     dx,E9_PORT
        cld
        or      ecx,ecx
        jz      pr_done
pr_next:
        lodsb
        cmp     al,32
        jb      pr_dot
        cmp     al,126
        jbe     pr_put
pr_dot:
        mov     al,'.'
pr_put:
        out     dx,al
        dec     ecx
        jnz     pr_next
pr_done:
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

pv_fail:
        or      word ptr [ebp+C_EFLAGS],1       ; caller's CF = 1: it did not
        mov     esi,OFFSET FLAT:pd_rv86
        call    pd_e9str
        mov     esi,OFFSET FLAT:pd_rfail
        call    pd_e9str
pv_done:
        popad
        ret

;---------------------------------------------------------------------
; pd_redir_trace - E9 one finished redirector slice: the op, the status,
; and the found entry's name, attribute and size (meaningful for a find;
; a mount leaves them zero).  Reads the bounce buffer; preserves all
; registers (so the client struct in EBP is untouched).
;---------------------------------------------------------------------
pd_redir_trace:
        pushad
        mov     esi,OFFSET FLAT:pd_rdop
        call    pd_e9str
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+NR_OP]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rdst
        call    pd_e9str
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+NR_STATUS]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rdnm
        call    pd_e9str
        mov     esi,[pd_pg]
        lea     esi,[esi+PGO_BUF+NR_NAME]
        call    pd_e9str
        mov     esi,OFFSET FLAT:pd_rdat
        call    pd_e9str
        mov     esi,[pd_pg]
        movzx   eax,byte ptr [esi+PGO_BUF+NR_ATTR]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rdsz
        call    pd_e9str
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+NR_SIZELO]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rdres         ; bytes moved (a read's answer)
        call    pd_e9str
        mov     esi,[pd_pg]
        mov     eax,[esi+PGO_BUF+NR_RESULT]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        popad
        ret

;---------------------------------------------------------------------
; pd_e9str - write the NUL-terminated string at DS:ESI to port E9h.
;---------------------------------------------------------------------
pd_e9str:
        push    eax
        push    edx
        push    esi
        mov     dx,E9_PORT
        cld
pe_next:
        lodsb
        or      al,al
        jz      pe_done
        out     dx,al
        jmp     pe_next
pe_done:
        pop     esi
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; pd_e9hex - write EAX to port E9h as eight hex digits, high first.
;---------------------------------------------------------------------
pd_e9hex:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx,eax
        mov     ecx,8
        mov     dx,E9_PORT
ph_next:
        rol     ebx,4
        mov     al,bl
        and     al,0Fh
        cmp     al,10
        jb      ph_dig
        add     al,'A' - 10
        jmp     ph_put
ph_dig:
        add     al,'0'
ph_put:
        out     dx,al
        dec     ecx
        jnz     ph_next
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; pd_e9mac - write ECX bytes at DS:ESI to port E9h as colon-separated
; hex pairs (a MAC address).
;---------------------------------------------------------------------
pd_e9mac:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        mov     dx,E9_PORT
        cld
pm_next:
        lodsb
        mov     bl,al
        mov     al,bl
        shr     al,4
        call    pm_nib
        mov     al,bl
        and     al,0Fh
        call    pm_nib
        dec     ecx
        jz      pm_done
        mov     al,':'
        out     dx,al
        jmp     pm_next
pm_done:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret
pm_nib:
        and     al,0Fh
        cmp     al,10
        jb      pm_dig
        add     al,'A' - 10
        jmp     pm_out
pm_dig:
        add     al,'0'
pm_out:
        out     dx,al
        ret

;---------------------------------------------------------------------
; pd_rzero - zero ECX bytes at ES:EDI.  pd_rcopy - copy the ASCIIZ string
; at DS:ESI to ES:EDI, at most 127 bytes and a terminator.  Resident
; (the async tick builds the BA2 request with them); pd_strcopy is the
; same but init-only, so it is not reached from the tick.
;---------------------------------------------------------------------
pd_rzero:
        push    eax
        xor     al,al
        cld
        rep     stosb
        pop     eax
        ret
pd_rcopy:
        push    eax
        push    ecx
        mov     ecx,127
        cld
prc_next:
        lodsb
        stosb
        or      al,al
        jz      prc_done
        dec     ecx
        jnz     prc_next
        xor     al,al
        stosb
prc_done:
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; pd_i13hook - the V86 INT 13h chain hook (Hook_V86_Int_Chain).  Entered
; at interrupt time, interrupts off, EBX = the current VM, EBP -> the
; Client_Reg_Struc.  For a USB disk this kernel serves (a bit set in
; pd_usbmask) the call is answered here, out of the kernel's own USB stack
; through the bridge, and EATEN with Simulate_Iret - so WfW never reflects
; it to the BIOS, which can no longer reach the controller and would hang
; or hand back a zero sector WfW then divides a CHS geometry by.  Any other
; drive is left to the chain (carry set).  The DDK's 386\INT13 is the model:
; not ours -> stc; ours -> Simulate_Iret, set the client regs + carry, clc.
; A fixed 63-sector, 16-head geometry, which matches the stick's own MBR and
; BPB, so CHS<->LBA is self-consistent; reads loop one sector a bridge call.
;---------------------------------------------------------------------
pd_i13hook:
        pushad
        ; TRACE (BC2 diag): 'i' for EVERY V86 INT 13h that reaches this hook,
        ; then the drive number's low nibble - shows whether WfW reflects
        ; INT 13h to the V86 chain at all, and for which drives.
        mov     dx,0E9h
        mov     al,'i'
        out     dx,al
        mov     al,byte ptr [ebp+C_EDX]
        shr     al,4
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     pdi_t1
        add     al,7
pdi_t1:
        out     dx,al
        mov     al,byte ptr [ebp+C_EDX]
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     pdi_t2
        add     al,7
pdi_t2:
        out     dx,al
        mov     eax,dword ptr [ebp+C_EDX]
        and     eax,0FFh                        ; DL = the drive
        cmp     al,80h
        jb      pdi_chain                       ; a diskette is never ours
        cmp     al,88h
        jae     pdi_chain
        mov     ecx,eax
        sub     ecx,80h
        mov     edx,1
        shl     edx,cl
        test    byte ptr [pd_usbmask],dl
        jz      pdi_chain

        ; TRACE (BC2): the hook fired for a USB drive - 'H' then the
        ; function digit (H2 = read, H8 = geometry), so a run shows WHETHER
        ; WfW's INT 13h for the stick reaches this V86 chain hook at all.
        mov     dx,0E9h
        mov     al,'H'
        out     dx,al
        mov     al,byte ptr [ebp+C_EAX+1]
        add     al,'0'
        out     dx,al

        VXDCALL S_SIMULATE_IRET                  ; eat the int: the VM returns past it
        movzx   eax,byte ptr [ebp+C_EAX+1]       ; AH = the function
        cmp     al,02h
        je      pdi_read
        cmp     al,08h
        je      pdi_geom
        cmp     al,41h
        je      pdi_noext
        cmp     al,15h
        je      pdi_type
        cmp     al,03h
        je      pdi_badcmd                       ; write: not served yet (init only reads)
        jmp     pdi_ok                           ; reset/status/verify/etc: succeed

pdi_chain:
        popad
        stc                                      ; not ours: reflect to the chain
        ret

pdi_ok:
        mov     byte ptr [ebp+C_EAX+1],0
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        jmp     pdi_done
pdi_badcmd:
        mov     byte ptr [ebp+C_EAX+1],01h       ; invalid command
        or      word ptr [ebp+C_EFLAGS],1
        jmp     pdi_done
pdi_noext:                                       ; INT 13h extensions: say no -> CHS used
        mov     byte ptr [ebp+C_EAX+1],01h
        or      word ptr [ebp+C_EFLAGS],1
        jmp     pdi_done
pdi_type:                                        ; AH=15h: a fixed disk is present
        mov     byte ptr [ebp+C_EAX+1],03h
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        jmp     pdi_done
pdi_geom:                                        ; AH=08h: 63 spt, 16 heads, 1024 cyl
        mov     word ptr [ebp+C_ECX],0FFFFh      ; CH = cyl low, CL = cyl-hi | 63 sectors
        mov     byte ptr [ebp+C_EDX+1],0Fh       ; DH = max head = 15 (16 heads)
        mov     eax,475h
        movzx   eax,byte ptr [eax]               ; BDA hard-disk count
        mov     byte ptr [ebp+C_EDX],al          ; DL = number of drives
        mov     byte ptr [ebp+C_EAX+1],0
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        jmp     pdi_done

pdi_read:
        mov     dword ptr [pd_i13done],0
        cmp     dword ptr [pd_pg],0
        je      pdi_rerr                         ; bridge not up yet: a clean error,
                                                 ; not zeroes - keeps WfW off the divide
        mov     eax,dword ptr [ebp+C_EDX]
        and     eax,0FFh
        mov     [pd_i13drv],eax
        movzx   ecx,word ptr [ebp+C_ECX]         ; CX
        mov     edx,ecx
        and     edx,3Fh                          ; sector 1..63
        mov     eax,ecx
        shr     eax,8
        and     eax,0FFh                         ; CH = cylinder low 8
        mov     ebx,ecx
        and     ebx,0C0h
        shl     ebx,2                            ; cylinder bits 8-9
        or      eax,ebx                          ; EAX = cylinder
        movzx   ebx,byte ptr [ebp+C_EDX+1]       ; DH = head
        shl     eax,4                            ; cyl * 16 heads
        add     eax,ebx                          ; + head
        imul    eax,eax,63                       ; * 63 sectors
        dec     edx
        add     eax,edx                          ; + (sector-1) = absolute LBA
        mov     [pd_i13lba],eax
        movzx   eax,byte ptr [ebp+C_EAX]         ; AL = sector count
        mov     [pd_i13cnt],eax
        mov     dword ptr [pd_i13done],0
        movzx   eax,word ptr [ebp+C_ES]
        shl     eax,4
        movzx   ebx,word ptr [ebp+C_EBX]
        add     eax,ebx                          ; ES:BX -> V86 linear buffer
        mov     [pd_i13buf],eax
        mov     ecx,[pd_i13cnt]
        shl     ecx,9
        add     ecx,eax                          ; the buffer's end
        cmp     ecx,PD_V86TOP
        ja      pdi_rerr                         ; runs past the VM's first megabyte
pdi_rloop:
        mov     eax,[pd_i13done]
        cmp     eax,[pd_i13cnt]
        jae     pdi_rok
        mov     esi,[pd_pg]
        mov     dword ptr [esi+PGO_BUF+BR_OP],0
        mov     eax,[pd_i13drv]
        mov     dword ptr [esi+PGO_BUF+BR_DRIVE],eax
        mov     eax,[pd_i13lba]
        mov     dword ptr [esi+PGO_BUF+BR_LBA],eax
        mov     dword ptr [esi+PGO_BUF+BR_COUNT],1
        mov     eax,NDTF_BLOCK
        xor     edx,edx
        xor     ebx,ebx
        mov     ecx,BR_SIZE
        mov     edi,PGO_BUF
        call    pd_bridge_call                   ; EBP (the client) is kept
        jc      pdi_rerr
        mov     esi,[pd_pg]
        cmp     dword ptr [esi+PGO_BUF+BR_STATUS],0
        jne     pdi_rerr
        lea     esi,[esi+PGO_BUF+BR_DATA]
        mov     edi,[pd_i13buf]
        mov     ecx,128                          ; 512 bytes
        cld
        rep     movsd
        inc     dword ptr [pd_i13lba]
        add     dword ptr [pd_i13buf],512
        inc     dword ptr [pd_i13done]
        jmp     pdi_rloop
pdi_rok:
        mov     eax,[pd_i13done]
        mov     byte ptr [ebp+C_EAX],al          ; AL = sectors read
        mov     byte ptr [ebp+C_EAX+1],0         ; AH = 0
        and     word ptr [ebp+C_EFLAGS],0FFFEh
        jmp     pdi_done
pdi_rerr:
        mov     eax,[pd_i13done]
        mov     byte ptr [ebp+C_EAX],al
        mov     byte ptr [ebp+C_EAX+1],04h       ; sector not found / read error
        or      word ptr [ebp+C_EFLAGS],1
pdi_done:
        popad
        clc                                      ; serviced: do not reflect
        ret

;=====================================================================
; STAGE D - the NDIS 3.0 MAC face.  The NDIS wrapper (NDIS.386) calls the
; handlers below through the characteristics table that the init segment
; registered.  A wrapper service that returns a value is reached by
; CALLing one of the thunks: VMM rewrites each 'int 20h; dd id' into a
; jump to the service, which returns to the thunk's caller (the 8000h bit
; in the id selects that call form).  The handlers are cdecl - arguments
; are pushed right to left, the caller clears the stack, the NDIS_STATUS
; result is returned in EAX - and preserve EBX, ESI, EDI, EBP.
;=====================================================================

; --- NDIS service thunks (CALL these) ---
nd_get_version: int     20h
                dd      NDIS_DYNLINK + NDIS_GET_VERSION
nd_reg_mac:     int     20h
                dd      NDIS_DYNLINK + NDIS_REGISTER_MAC
nd_dereg:       int     20h
                dd      NDIS_DYNLINK + NDIS_DEREGISTER
nd_reg_adapter: int     20h
                dd      NDIS_DYNLINK + NDIS_REGISTER_ADAPTER
nd_reg_adap2:   int     20h
                dd      NDIS_DYNLINK + NDIS_REGISTER_ADAP2
nd_open_cfg:    int     20h
                dd      NDIS_DYNLINK + NDIS_OPEN_CONFIG
nd_close_cfg:   int     20h
                dd      NDIS_DYNLINK + NDIS_CLOSE_CONFIG
nd_qpacket:     int     20h                      ; NdisQueryPacket(pkt,0,0,&firstbuf,0)
                dd      NDIS_DYNLINK + NDIS_QUERY_PACKET
nd_qbuffer:     int     20h                      ; NdisQueryBuffer(buf,&vaddr,&len)
                dd      NDIS_DYNLINK + NDIS_QUERY_BUFFER
nd_nextbuf:     int     20h                      ; NdisGetNextBuffer(buf,&nextbuf)
                dd      NDIS_DYNLINK + NDIS_NEXT_BUFFER
nd_ind_recv:    int     20h                      ; EthFilterIndicateReceive (8 args)
                dd      NDIS_DYNLINK + NDIS_IND_RECEIVE
nd_ind_done:    int     20h                      ; EthFilterIndicateReceiveComplete (1 arg)
                dd      NDIS_DYNLINK + NDIS_IND_RECV_DONE

;---------------------------------------------------------------------
; pd_rx_pump - D1c receive.  The parked stack holds every frame it does
; not own (NetBEUI and the like) in a ring; take them one at a time into
; the low page (SOCK_RXFOREIGN -> PGO_RXBUF, EAX = length, 0 = none) and
; indicate each to the bound protocol, then signal the batch complete.
; Called from the tick and the card ISR, just after SOCK_PUMP has run the
; stack over whatever the card delivered.  Runs at interrupt time, which
; is where a real MAC indicates receives from; ndis.386 serialises.
;---------------------------------------------------------------------
pd_rx_pump:
        cmp     dword ptr [pd_bind_used],0      ; no protocol bound: nothing to do
        je      prp_ret
        cmp     byte ptr [pd_rx_busy],0         ; never re-enter
        jne     prp_ret
        mov     byte ptr [pd_rx_busy],1
        mov     byte ptr [pd_rx_any],0
        mov     dword ptr [pd_rx_cnt],0
prp_loop:
        mov     eax,NDTF_STACK
        xor     edx,edx
        mov     ebx,SOCK_RXFOREIGN
        mov     ecx,NIC_MAXFRAME
        mov     edi,PGO_RXBUF
        call    pd_bridge_call                  ; -> EAX = frame length, 0 = empty
        jc      prp_done                        ; bridge not ready / busy
        test    eax,eax
        jz      prp_done
        cmp     eax,NIC_HDRLEN
        jb      prp_next                        ; a runt: skip, try the next
        mov     [pd_rx_len],eax
        mov     byte ptr [pd_rx_any],1
        call    pd_rx_indicate
prp_next:
        inc     dword ptr [pd_rx_cnt]
        cmp     dword ptr [pd_rx_cnt],32        ; a cap, so one drain can't run away
        jb      prp_loop
prp_done:
        cmp     byte ptr [pd_rx_any],0
        je      prp_clr
        push    dword ptr [pd_bind_nbc]         ; EthFilterIndicateReceiveComplete(binding)
        call    nd_ind_done
        add     esp,4
prp_clr:
        mov     byte ptr [pd_rx_busy],0
prp_ret:
        ret

;---------------------------------------------------------------------
; pd_rx_indicate - hand the frame now in the low page at PGO_RXBUF to the
; bound protocol through EthFilterIndicateReceive (svc 3Dh).  The whole
; frame is offered as lookahead, so the protocol can take it directly or
; call MacTransferData (mac_xfer) to copy it; both read from PGO_RXBUF.
;   arg1 &status  arg2 NdisBindingContext  arg3 MacReceiveContext
;   arg4 HeaderBuffer  arg5 HeaderBufferSize  arg6 LookaheadBuffer
;   arg7 LookaheadBufferSize  arg8 PacketSize
; ndis.386 forwards args 3..8 to the protocol's receive handler with the
; protocol's own binding context; the header is the 14-byte Ethernet
; header, the lookahead and packet size are the bytes after it.
;---------------------------------------------------------------------
pd_rx_indicate:
        pushad
        cmp     byte ptr [pd_rx_traced],0
        jne     pri_nt
        mov     byte ptr [pd_rx_traced],1
        mov     esi,OFFSET FLAT:pd_msg_rx
        call    pd_e9str
pri_nt:
        mov     esi,[pd_pg]
        add     esi,PGO_RXBUF                   ; ESI = frame base, linear (1:1 page)
        mov     ecx,[pd_rx_len]
        sub     ecx,NIC_HDRLEN                  ; ECX = bytes after the header
        push    ecx                             ; arg8 PacketSize
        push    ecx                             ; arg7 LookaheadBufferSize (whole frame)
        lea     eax,[esi+NIC_HDRLEN]
        push    eax                             ; arg6 LookaheadBuffer
        push    NIC_HDRLEN                      ; arg5 HeaderBufferSize
        push    esi                             ; arg4 HeaderBuffer
        push    esi                             ; arg3 MacReceiveContext (mac_xfer uses globals)
        push    dword ptr [pd_bind_nbc]         ; arg2 NdisBindingContext
        push    OFFSET FLAT:pd_rx_status        ; arg1 &status (out)
        call    nd_ind_recv                     ; svc 3Dh
        add     esp,20h
        popad
        ret

;---------------------------------------------------------------------
; mac_open - MacOpenAdapter.  A protocol (NetBEUI) binds to the adapter.
;   [ebp+08] &OpenErrorStatus  [ebp+0C] &MacBindingHandle
;   [ebp+10] &SelectedMediumIndex  [ebp+14] MediumArray  [ebp+18] count
;   [ebp+1C] NdisBindingContext  [ebp+20] MacAdapterContext
; Select NdisMedium802_3 (value 0) from the medium array, record the one
; binding and hand back its record as the MacBindingHandle.
;---------------------------------------------------------------------
mac_open:
        push    ebp
        mov     ebp,esp
        push    esi
        pushad
        mov     esi,OFFSET FLAT:pd_msgmoc
        call    pd_e9str
        popad
        mov     eax,[ebp+08h]
        mov     dword ptr [eax],0               ; *OpenErrorStatus = SUCCESS
        mov     eax,[ebp+14h]                   ; MediumArray
        mov     edx,[ebp+18h]                   ; count
        mov     ecx,0FFFFFFFFh                  ; selected index = none
        xor     esi,esi
mo_scan:
        test    edx,edx
        jz      mo_scandone
        cmp     dword ptr [eax],0               ; NdisMedium802_3?
        jne     mo_scannext
        mov     ecx,esi
        jmp     mo_scandone
mo_scannext:
        add     eax,4
        inc     esi
        dec     edx
        jmp     mo_scan
mo_scandone:
        mov     eax,[ebp+10h]
        mov     [eax],ecx                       ; *SelectedMediumIndex
        cmp     ecx,0FFFFFFFFh
        jne     mo_haveidx
        mov     eax,NDIS_UNSUPPORTED_MEDIA
        jmp     mo_ret
mo_haveidx:
        cmp     dword ptr [pd_bind_used],0
        jne     mo_busy
        mov     dword ptr [pd_bind_used],1
        mov     eax,[ebp+1Ch]
        mov     [pd_bind_nbc],eax               ; NdisBindingContext
        mov     eax,[ebp+20h]
        mov     [pd_bind_adap],eax              ; MacAdapterContext
        mov     eax,[ebp+0Ch]
        mov     dword ptr [eax],OFFSET FLAT:pd_bind_used   ; the binding handle
        mov     esi,OFFSET FLAT:pd_msgopen
        call    pd_e9str
        xor     eax,eax                         ; NDIS_SUCCESS
        jmp     mo_ret
mo_busy:
        mov     eax,NDIS_FAILURE
mo_ret:
        pop     esi
        pop     ebp
        ret

;---------------------------------------------------------------------
; mac_close - MacCloseAdapter( MacBindingHandle, ... ).  Free the binding.
;---------------------------------------------------------------------
mac_close:
        pushad
        mov     esi,OFFSET FLAT:pd_msg_cls
        call    pd_e9str
        popad
        mov     dword ptr [pd_bind_used],0
        xor     eax,eax
        ret

;---------------------------------------------------------------------
; mac_reset - MacReset( MacBindingHandle ).  No hardware reset is needed
; (the kernel owns the card); complete synchronously.
;---------------------------------------------------------------------
mac_reset:
        pushad
        mov     esi,OFFSET FLAT:pd_msg_rst
        call    pd_e9str
        popad
        xor     eax,eax
        ret

;---------------------------------------------------------------------
; mac_send - MacSend( MacBindingHandle, Packet ).  D1b: walk the NDIS
; packet's buffer chain into the low-page TX frame, stamp our source MAC
; into it, and hand the whole Ethernet frame to the parked kernel's NIC
; driver (NIC_SEND).  The send is synchronous, so return NDIS_SUCCESS.
;   [ebp+08] MacBindingHandle   [ebp+0C] Packet
;---------------------------------------------------------------------
mac_send:
        push    ebp
        mov     ebp,esp
        sub     esp,10h                         ; -4 buf, -8 vaddr, -0Ch len, -10h total
        push    ebx
        push    esi
        push    edi
        cmp     byte ptr [pd_ms_traced],0
        jne     mstx_nt
        mov     byte ptr [pd_ms_traced],1
        pushad
        mov     esi,OFFSET FLAT:pd_msg_tx
        call    pd_e9str
        popad
mstx_nt:
        ; NdisQueryPacket( Packet, 0, 0, &FirstBuffer, 0 )
        push    0
        lea     eax,[ebp-4]
        push    eax
        push    0
        push    0
        mov     eax,[ebp+0Ch]
        push    eax
        call    nd_qpacket
        add     esp,14h
        mov     dword ptr [ebp-10h],0           ; total = 0
mstx_loop:
        mov     esi,[ebp-4]                     ; buf
        test    esi,esi
        jz      mstx_send
        ; NdisQueryBuffer( buf, &vaddr, &len )
        lea     eax,[ebp-0Ch]
        push    eax
        lea     eax,[ebp-8]
        push    eax
        push    esi
        call    nd_qbuffer
        add     esp,0Ch
        mov     ecx,[ebp-0Ch]                   ; len
        mov     eax,[ebp-10h]                   ; total
        add     eax,ecx
        jc      mstx_drop
        cmp     eax,NIC_MAXFRAME
        ja      mstx_drop                       ; oversized frame: drop it
        mov     esi,[ebp-8]                     ; src = vaddr
        mov     edi,[pd_pg]
        add     edi,PGO_TXBUF
        add     edi,[ebp-10h]                   ; dst = page + PGO_TXBUF + total
        cld
        rep     movsb
        mov     eax,[ebp-0Ch]
        add     [ebp-10h],eax                   ; total += len
        mov     esi,[ebp-4]
        lea     eax,[ebp-4]                     ; &NextBuffer -> buf
        push    eax
        push    esi
        call    nd_nextbuf
        add     esp,8
        jmp     mstx_loop
mstx_send:
        mov     ecx,[ebp-10h]                   ; total
        test    ecx,ecx
        jz      mstx_drop
        cmp     ecx,NIC_HDRLEN                  ; a header at least
        jb      mstx_drop
        ; stamp our source MAC (the kernel card's, from NIC_INFO) at [6..11]
        mov     edi,[pd_pg]
        add     edi,PGO_TXBUF
        add     edi,6
        mov     esi,OFFSET FLAT:pd_macaddr
        mov     ecx,6
        cld
        rep     movsb
        ; NIC_SEND: EDI (page-relative) -> frame of ECX bytes
        mov     eax,NDTF_NIC
        xor     edx,edx
        mov     ebx,NIC_SEND
        mov     ecx,[ebp-10h]
        mov     edi,PGO_TXBUF
        call    pd_bridge_call                  ; TX error just drops the frame
mstx_drop:
        xor     eax,eax                         ; NDIS_SUCCESS (packet consumed)
        pop     edi
        pop     esi
        pop     ebx
        mov     esp,ebp
        pop     ebp
        ret

;---------------------------------------------------------------------
; mac_xfer - MacTransferData.  D1c: the protocol asks for BytesToTransfer
; bytes of the just-indicated frame, from ByteOffset past the Ethernet
; header, copied into its packet.  The frame is in the low page at
; PGO_RXBUF; walk the packet's NDIS buffer chain and copy into it.
;   [ebp+08] MacBindingHandle   [ebp+0C] MacReceiveContext
;   [ebp+10] ByteOffset         [ebp+14] BytesToTransfer
;   [ebp+18] Packet             [ebp+1C] &BytesTransferred
; Returns NDIS_SUCCESS; *BytesTransferred = the bytes actually copied.
;---------------------------------------------------------------------
mac_xfer:
        push    ebp
        mov     ebp,esp
        sub     esp,10h                         ; -4 buf, -8 vaddr, -0Ch buflen, -10h copied
        push    ebx
        push    esi
        push    edi
        cmp     byte ptr [pd_rx_xtraced],0
        jne     mx_nt
        mov     byte ptr [pd_rx_xtraced],1
        pushad
        mov     esi,OFFSET FLAT:pd_msg_xfer
        call    pd_e9str
        popad
mx_nt:
        ; NdisQueryPacket( Packet, 0, 0, &FirstBuffer, 0 )
        push    0
        lea     eax,[ebp-4]
        push    eax
        push    0
        push    0
        mov     eax,[ebp+18h]                   ; Packet
        push    eax
        call    nd_qpacket
        add     esp,14h
        mov     dword ptr [ebp-10h],0           ; copied = 0
mx_loop:
        mov     esi,[ebp-4]                     ; current buffer
        test    esi,esi
        jz      mx_fin
        mov     edx,[ebp+14h]                   ; remaining = BytesToTransfer - copied
        sub     edx,[ebp-10h]
        jbe     mx_fin                          ; all the protocol asked for is copied
        mov     eax,[pd_rx_len]                 ; available = len - header - offset - copied
        sub     eax,NIC_HDRLEN
        cmp     eax,[ebp+10h]
        jbe     mx_fin                          ; the offset is past the frame
        sub     eax,[ebp+10h]
        cmp     eax,[ebp-10h]
        jbe     mx_fin                          ; nothing left in the frame
        sub     eax,[ebp-10h]
        cmp     edx,eax
        jbe     mx_capped
        mov     edx,eax                         ; cap remaining to what the frame holds
mx_capped:
        ; NdisQueryBuffer( buf, &vaddr, &len )
        lea     eax,[ebp-0Ch]
        push    eax
        lea     eax,[ebp-8]
        push    eax
        push    esi
        call    nd_qbuffer
        add     esp,0Ch
        mov     ecx,[ebp-0Ch]                   ; this buffer's room
        test    ecx,ecx
        jz      mx_next
        cmp     ecx,edx                         ; copy min( room, remaining )
        jbe     mx_cok
        mov     ecx,edx
mx_cok:
        mov     esi,[pd_pg]                     ; src = page + PGO_RXBUF + header + offset + copied
        add     esi,PGO_RXBUF
        add     esi,NIC_HDRLEN
        add     esi,[ebp+10h]
        add     esi,[ebp-10h]
        mov     edi,[ebp-8]                     ; dst = this buffer's address
        push    ecx
        cld
        rep     movsb
        pop     ecx
        add     [ebp-10h],ecx                   ; copied += ecx
mx_next:
        mov     esi,[ebp-4]
        lea     eax,[ebp-4]                     ; &NextBuffer -> buf
        push    eax
        push    esi
        call    nd_nextbuf
        add     esp,8
        jmp     mx_loop
mx_fin:
        mov     eax,[ebp+1Ch]                   ; *BytesTransferred = copied
        mov     ecx,[ebp-10h]
        mov     [eax],ecx
        xor     eax,eax                         ; NDIS_SUCCESS
        pop     edi
        pop     esi
        pop     ebx
        mov     esp,ebp
        pop     ebp
        ret

;---------------------------------------------------------------------
; mac_request - MacRequest( MacBindingHandle, NDIS_REQUEST ).  Answer the
; generic/802.3 OID queries NetBEUI makes at bind, and store the settable
; ones.  The query answers come from pd_oidtab {OID, length, source}.
;---------------------------------------------------------------------
mac_request:
        push    ebp
        mov     ebp,esp
        push    ebx
        push    esi
        push    edi
        cmp     byte ptr [pd_mr_traced],0
        jne     mr_notrace
        mov     byte ptr [pd_mr_traced],1
        pushad
        mov     esi,OFFSET FLAT:pd_msgmrc
        call    pd_e9str
        popad
mr_notrace:
        mov     ebx,[ebp+0Ch]                   ; NDIS_REQUEST
        cmp     dword ptr [ebx+NRQ_TYPE],0
        jne     mr_set
        ; ---- QueryInformation ----
        mov     eax,[ebx+NRQ_OID]
        mov     esi,OFFSET FLAT:pd_oidtab
mr_scan:
        mov     ecx,[esi]                       ; table OID
        test    ecx,ecx
        jz      mr_notsup                       ; end marker
        cmp     ecx,eax
        je      mr_found
        add     esi,12
        jmp     mr_scan
mr_found:
        mov     edi,[esi+4]                     ; length
        mov     esi,[esi+8]                     ; source pointer
        mov     eax,[ebx+NRQ_BUFLEN]
        cmp     edi,eax
        jbe     mr_fits
        mov     [ebx+NRQ_NEEDED],edi
        mov     dword ptr [ebx+NRQ_WRITTEN],0
        mov     eax,NDIS_BAD_LENGTH
        jmp     mr_done
mr_fits:
        mov     [ebx+NRQ_WRITTEN],edi
        mov     edx,[ebx+NRQ_BUFFER]            ; destination
mr_cpy:
        test    edi,edi
        jz      mr_cpydone
        mov     al,[esi]
        mov     [edx],al
        inc     esi
        inc     edx
        dec     edi
        jmp     mr_cpy
mr_cpydone:
        xor     eax,eax                         ; NDIS_SUCCESS
        jmp     mr_done
mr_notsup:
        mov     dword ptr [ebx+NRQ_WRITTEN],0
        mov     eax,NDIS_NOT_SUPPORTED
        jmp     mr_done
mr_set:
        ; ---- SetInformation ----  accept the settable OIDs
        mov     eax,[ebx+NRQ_OID]
        cmp     dword ptr [ebx+NRQ_BUFLEN],4
        jb      mr_setbad
        mov     edx,[ebx+NRQ_BUFFER]
        cmp     eax,OID_GEN_CURRENT_PACKET_FILTER
        jne     mr_set1
        mov     ecx,[edx]
        mov     [pd_pktfilter],ecx
        jmp     mr_setok
mr_set1:
        cmp     eax,OID_GEN_CURRENT_LOOKAHEAD
        jne     mr_set2
        mov     ecx,[edx]
        mov     [pd_lookahead],ecx
        jmp     mr_setok
mr_set2:
        cmp     eax,OID_802_3_MULTICAST_LIST
        je      mr_setok                        ; we indicate all; accept
        cmp     eax,OID_GEN_MAC_OPTIONS
        je      mr_setok
mr_setbad:
        mov     dword ptr [ebx+NRQ_WRITTEN],0
        mov     eax,NDIS_NOT_SUPPORTED
        jmp     mr_done
mr_setok:
        mov     eax,[ebx+NRQ_BUFLEN]
        mov     [ebx+NRQ_WRITTEN],eax
        xor     eax,eax
mr_done:
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

;---------------------------------------------------------------------
; mac_qstats - QueryGlobalStatistics( MacAdapterContext, NDIS_REQUEST ).
; D1a: statistics are optional; report none.
;---------------------------------------------------------------------
mac_qstats:
        pushad
        mov     esi,OFFSET FLAT:pd_msg_qs
        call    pd_e9str
        popad
        push    ebp
        mov     ebp,esp
        mov     edx,[ebp+0Ch]
        mov     dword ptr [edx+NRQ_WRITTEN],0
        mov     eax,NDIS_NOT_SUPPORTED
        pop     ebp
        ret

;---------------------------------------------------------------------
; mac_unload / mac_addadap / mac_remadap - the MAC-driver lifecycle.
; Device_Init registers the MAC (svc 31h) and its characteristics (svc 33h);
; ndis.386 then calls mac_addadap, and the bindable adapter instance is
; created THERE with svc 35h (NdisRegisterAdapter).  Registering only at
; Device_Init is not enough - without the svc 35h call ndis.386 has no
; adapter to offer a protocol, so NetBEUI never binds and mac_open never runs.
;---------------------------------------------------------------------
mac_unload:
        pushad
        mov     esi,OFFSET FLAT:pd_msg_unl
        call    pd_e9str
        popad
        xor     eax,eax
        ret
; MacAddAdapter( MacMacContext, ConfigHandle, &AdapterName ).
;   [ebp+08] MacMacContext  [ebp+0C] ConfigHandle  [ebp+10] &AdapterName
mac_addadap:
        push    ebp
        mov     ebp,esp
        pushad
        mov     esi,OFFSET FLAT:pd_msg_aa
        call    pd_e9str
        mov     eax,[ebp+08h]                   ; MacMacContext
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_sp
        call    pd_e9str
        mov     eax,[ebp+0Ch]                   ; ConfigHandle
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_sp
        call    pd_e9str
        mov     eax,[ebp+10h]                   ; &AdapterName
        call    pd_e9hex
        ; deref &AdapterName as an NDIS_STRING: len/maxlen word, then buffer
        mov     esi,OFFSET FLAT:pd_msg_nm
        call    pd_e9str
        mov     esi,[ebp+10h]
        mov     eax,[esi]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_sp
        call    pd_e9str
        mov     esi,[ebp+10h]
        mov     eax,[esi+4]
        call    pd_e9hex
        popad

        ; svc 05h/07h - open then close the card's PROTOCOL.INI section with
        ; the ConfigHandle ndis.386 passed.  A real NIC reads IOBASE/INTERRUPT
        ; here; the kernel card has none, but the in-box MACs always open the
        ; configuration in AddAdapter, which is what ties the adapter instance
        ; to its config section so the protocol manager can bind to it.
        mov     eax,[ebp+0Ch]                   ; ConfigHandle
        push    eax
        push    OFFSET FLAT:pd_ndis_cfgh        ; &cfgHandle (out)
        push    OFFSET FLAT:pd_ndis_cfgstatus   ; &status (out)
        call    nd_open_cfg
        add     esp,0Ch
        pushad
        mov     esi,OFFSET FLAT:pd_msg_cfg
        call    pd_e9str
        mov     eax,[pd_ndis_cfgstatus]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_msg_cfgh
        call    pd_e9str
        mov     eax,[pd_ndis_cfgh]
        call    pd_e9hex
        popad
        cmp     dword ptr [pd_ndis_cfgstatus],0
        jnz     pd_aa_noclose
        mov     eax,[pd_ndis_cfgh]
        push    eax
        call    nd_close_cfg
        add     esp,4
pd_aa_noclose:

        ; svc 35h - register the bindable adapter instance.  The kernel card
        ; has no PROTOCOL.INI hardware keys to read, so (unlike a real NIC's
        ; AddAdapter) we skip the config read and register straight away with
        ; the char handle svc 33h produced, our own context, and the name
        ; ndis.386 passed; the optional info struct is NULL.
        ; NdisRegisterAdapter( &out, charhandle, ctx, cfg, &name, 0 )
        push    0                               ; info struct (optional) = NULL
        mov     eax,[ebp+10h]                   ; arg5 &AdapterName (NDIS_STRING)
        push    eax
        mov     eax,[ebp+0Ch]                   ; arg4 ConfigHandle (svc 35h ignores it)
        push    eax
        push    OFFSET FLAT:pd_adapter          ; arg3 MacAdapterContext (nonzero)
        push    dword ptr [pd_ndis_ctx]         ; arg2 the char handle from svc 33h
        push    OFFSET FLAT:pd_ndis_adapterobj  ; arg1 out: the registered adapter
        call    nd_reg_adap2
        add     esp,18h
        mov     [pd_ndis_aastatus],eax

        pushad
        mov     esi,OFFSET FLAT:pd_msg_aard
        call    pd_e9str
        mov     eax,[pd_ndis_aastatus]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        popad
        mov     eax,[pd_ndis_aastatus]          ; AddAdapter returns svc 35h's status
        mov     esp,ebp
        pop     ebp
        ret
mac_remadap:
        xor     eax,eax
        ret

_LTEXT  ENDS

;=====================================================================
; INIT CODE - run once at Device_Init, then discarded.  It brings the
; bridge up (reads the parked kernel's landmarks, allocates the low
; page, installs the trampoline, copies WIN386's GDT) and proves it
; with one NIC_INFO call before handing over to the resident API.
;=====================================================================
_ITEXT  SEGMENT
        ASSUME  ds:FLAT, es:FLAT, ss:FLAT

pd_devinit:
        pushad                                  ; VMM's registers (EBX = the
                                                ; System VM) are preserved
        mov     esi,OFFSET FLAT:pd_hello
        call    pd_e9str

        ; The parked kernel's globals, read 1:1 - CR3 and the bridge
        ; signature/entry it published.
        mov     esi,OFFSET FLAT:pd_msgpgd
        call    pd_e9str
        mov     ebx,KL_GV_PGDIR
        mov     eax,[ebx]
        mov     [pd_kcr3],eax
        call    pd_e9hex

        mov     ebx,KL_GV_BRGSIG
        mov     eax,[ebx]
        cmp     eax,BRG_SIGVAL
        jne     pd_nobridge
        mov     ebx,KL_GV_BRGENTRY
        mov     eax,[ebx]
        mov     [pd_kentry],eax
        mov     esi,OFFSET FLAT:pd_msgent
        call    pd_e9str
        call    pd_e9hex

        ; A low page from the VMM global V86 data area: below 1MB, so
        ; 1:1 under both maps - the only safe home for the switch stub,
        ; its scratch stack and the bounce buffer.  The service has a
        ; leading underscore in the DDK, so it is C-callable (args pushed
        ; right to left, the caller clears the stack), not register-based.
        push    GVDA_PAGEALIGN OR GVDA_ZEROINIT ; flags (2nd arg)
        push    2000h                           ; nBytes - 2 pages: page 1 is the
                                                ; stub/cells/bounce, page 2 the NDIS
                                                ; TX/RX frame buffers (1:1, contiguous)
        VXDCALL S_ALLOC_GV86
        add     esp,8
        or      eax,eax
        jz      pd_noalloc
        mov     [pd_pg],eax
        mov     esi,OFFSET FLAT:pd_msgpg
        call    pd_e9str
        mov     eax,[pd_pg]
        call    pd_e9hex

        ; Install the trampoline at the page's start.
        mov     edi,[pd_pg]
        mov     esi,OFFSET FLAT:pd_tramp_src
        mov     ecx,pd_tramp_end - pd_tramp_src
        cld
        rep     movsb

        ; Fill the page's fixed cells: the kernel's CR3 and brg_entry.
        mov     ebp,[pd_pg]
        mov     eax,[pd_kcr3]
        mov     [ebp+PGO_KCR3],eax
        mov     eax,[pd_kentry]
        mov     [ebp+PGO_KENTRY],eax

        ; Copy WIN386's GDT into the low page and build a GDTR for it.  A
        ; segment load during the window (the driver's 'pop es') makes the
        ; CPU fetch a descriptor at the GDTR's base; WIN386's GDT is at a
        ; high linear address unmapped under the kernel's CR3, so the
        ; window runs on this low, 1:1 copy instead - the descriptors are
        ; identical, so every already-loaded selector stays valid.
        sgdt    [ebp+PGO_WGDTR]
        movzx   ecx,word ptr [ebp+PGO_WGDTR]    ; the limit
        inc     ecx                             ; bytes in the GDT
        cmp     ecx,PGO_GDTMAX
        ja      pd_gdtbig                       ; no room for it under the stack
        mov     esi,[ebp+PGO_WGDTR+2]           ; WIN386's GDT base
        lea     edi,[ebp+PGO_GDT]
        push    ds
        pop     es
        cld
        rep     movsb
        mov     ax,word ptr [ebp+PGO_WGDTR]     ; the low GDTR: same limit,
        mov     word ptr [ebp+PGO_LGDTR],ax     ; base = the copy
        lea     eax,[ebp+PGO_GDT]
        mov     dword ptr [ebp+PGO_LGDTR+2],eax

        ; Self-test: one NIC_INFO through the bridge, as Stage A proved.
        mov     esi,OFFSET FLAT:pd_msgcall
        call    pd_e9str
        mov     eax,NDTF_NIC
        xor     edx,edx
        mov     ebx,NIC_INFO
        mov     ecx,NICI_LEN
        mov     edi,PGO_BUF
        call    pd_bridge_call
        push    eax
        call    pd_e9hex                        ; the answer (NICI_LEN = 34h)
        pop     eax

        mov     esi,OFFSET FLAT:pd_msgmac
        call    pd_e9str
        mov     ebp,[pd_pg]
        lea     esi,[ebp+PGO_BUF+NICI_MAC]
        mov     ecx,6
        call    pd_e9mac
        ; keep the card's MAC for the NDIS OID answers (the USB probe below
        ; reuses PGO_BUF, so copy it out now).
        mov     ebp,[pd_pg]
        lea     esi,[ebp+PGO_BUF+NICI_MAC]
        mov     edi,OFFSET FLAT:pd_macaddr
        mov     ecx,6
        cld
        rep     movsb
        mov     esi,OFFSET FLAT:pd_msgname
        call    pd_e9str
        mov     ebp,[pd_pg]
        lea     esi,[ebp+PGO_BUF+NICI_NAME]
        call    pd_e9str
        mov     esi,OFFSET FLAT:pd_crlf
        call    pd_e9str

        ;=============================================================
        ; STAGE B, BA1 - bring the parked network up under Windows and
        ; prove it runs, driven only by the bridge's async tick.
        ;=============================================================

        ; The card's line, as the self-test's NIC_INFO just reported it.
        ; NIC_NOIRQ (0FFh), or 0, means the card is polled; a real line is
        ; what BA1b hands to VPICD so a received frame reaches the parked
        ; stack the instant it lands, not a tick later.
        mov     ebp,[pd_pg]
        movzx   eax,byte ptr [ebp+PGO_BUF+NICI_IRQ]
        mov     [pd_nicirq],al
        mov     esi,OFFSET FLAT:pd_msgirq
        call    pd_e9str
        call    pd_e9hex

        ; Take the line from VPICD and wake the card with its interrupt
        ; live.  If the card is polled, or VPICD will not give the line
        ; (another device has it), fall back to the polled wake: the tick
        ; alone then drains the card, a tick later at worst.  The order is
        ; deliberate - virtualize and unmask the PIC while the card's own
        ; interrupt mask is still clear (the park stopped it), THEN wake
        ; the card, so there is no moment when it asserts with no handler.
        cmp     byte ptr [pd_nicirq],NIC_NOIRQ
        je      pd_wakepolled
        cmp     byte ptr [pd_nicirq],0
        je      pd_wakepolled
        movzx   eax,byte ptr [pd_nicirq]
        mov     word ptr [pd_vid],ax            ; the line into the descriptor
        mov     edi,OFFSET FLAT:pd_vid
        VXDCALL S_VPICD_VIRTUALIZE_IRQ
        jc      pd_wakepolled                   ; the line is taken: polled
        mov     [pd_nich],eax                   ; VPICD's handle
        VXDCALL S_VPICD_PHYS_UNMASK             ; EAX = the handle: let it in
        mov     esi,OFFSET FLAT:pd_msgvirq
        call    pd_e9str
        mov     eax,[pd_nich]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_msgwkirq
        call    pd_e9str
        mov     eax,NDTF_NIC
        xor     edx,edx
        mov     ebx,NIC_WAKE_IRQ
        xor     ecx,ecx
        mov     edi,PGO_BUF
        call    pd_bridge_call
        call    pd_e9hex                        ; 1 if the card is running
        jmp     pd_wokedone

pd_wakepolled:
        ; The park stopped the card and cleared its running flag before
        ; this VxD's Device_Init; NIC_WAKE sets it up again, polled (its
        ; interrupt masked, so nothing is left unserviced under Windows),
        ; and marks it running so NIC_SERVICE drains the ring.
        mov     esi,OFFSET FLAT:pd_msgwake
        call    pd_e9str
        mov     eax,NDTF_NIC
        xor     edx,edx
        mov     ebx,NIC_WAKE
        xor     ecx,ecx
        mov     edi,PGO_BUF
        call    pd_bridge_call
        call    pd_e9hex                        ; 1 if the card is running
pd_wokedone:

        ; Prepare the self-test's name lookup: put its SOCK_RES (the name
        ; to resolve) in the persistent low-page buffer.  The lookup is
        ; not started here - DHCP may not have finished before the park,
        ; so the tick waits for the interface to come up (SOCK_IFSTAT),
        ; then starts the lookup and polls it to DONE (an address came
        ; back) or FAIL (no answer after the stack's own retries, which
        ; itself proves the stack's clock and timers ran under Windows).
        mov     ebp,[pd_pg]
        lea     edi,[ebp+PGO_RES]               ; zero the persistent SOCK_RES
        xor     eax,eax
        mov     ecx,SOCKRES_LEN / 4
        cld
        rep     stosd
        lea     edi,[ebp+PGO_RES+SR_NAME]       ; the name to look up
        mov     esi,OFFSET FLAT:pd_resname
        call    pd_strcopy
        mov     byte ptr [pd_ba1],0
        mov     esi,OFFSET FLAT:pd_msgkick
        call    pd_e9str

        ; BC2 - the cure for the USB splash hang.  Find which BIOS hard-disk
        ; numbers (80h..87h) are USB disks this kernel serves: blk_bridge's
        ; query op (BR_OP=2) answers from usbm_owns, which claims the stick's
        ; INT 13h alias (81h) because the BIOS can no longer read it.  Then
        ; hook V86 INT 13h so WfW's reads of those drives come here, not to
        ; the BIOS that would hang / hand back a zero sector.
        mov     byte ptr [pd_usbmask],0
        mov     ecx,80h
pdv_disc:
        cmp     ecx,88h
        jae     pdv_dischook
        mov     ebp,[pd_pg]
        mov     dword ptr [ebp+PGO_BUF+BR_OP],2
        mov     dword ptr [ebp+PGO_BUF+BR_DRIVE],ecx
        push    ecx
        mov     eax,NDTF_BLOCK
        xor     edx,edx
        xor     ebx,ebx
        mov     ecx,BR_SIZE
        mov     edi,PGO_BUF
        call    pd_bridge_call
        pop     ecx
        jc      pdv_discnext
        mov     ebp,[pd_pg]
        cmp     dword ptr [ebp+PGO_BUF+BR_STATUS],0
        jne     pdv_discnext
        push    ecx
        sub     ecx,80h
        mov     eax,1
        shl     eax,cl
        or      byte ptr [pd_usbmask],al
        pop     ecx
pdv_discnext:
        inc     ecx
        jmp     pdv_disc
pdv_dischook:
        mov     esi,OFFSET FLAT:pd_msgi13
        call    pd_e9str
        movzx   eax,byte ptr [pd_usbmask]
        call    pd_e9hex
        mov     esi,OFFSET FLAT:pd_rcrlf
        call    pd_e9str
        ; the hook is already installed (pd_syscrit); the discovery above
        ; has just narrowed its drive mask to what usbm_owns really claims.

        ;=============================================================
        ; STAGE D, D1 - register with the NDIS 3.0 wrapper (NDIS.386) as a
        ; native MAC, so WfW's own protocols bind to the kernel-owned card.
        ; Skipped cleanly when no NDIS wrapper is loaded (a non-net build):
        ; Get_Version carries 8000h, so an absent device returns CF rather
        ; than faulting, and Stages A-C are left exactly as before.
        ;=============================================================
        call    nd_get_version                  ; NDIS present?
        jc      pd_nondis                        ; CF = no NDIS wrapper
        mov     esi,OFFSET FLAT:pd_msgndis
        call    pd_e9str

        ; NdisRegisterMac( &handle, &descriptor, 0, 0 ) -> the MAC handle
        push    0
        push    0
        push    OFFSET FLAT:pd_reg_desc
        push    OFFSET FLAT:pd_ndis_mach
        call    nd_reg_mac
        add     esp,16
        mov     esi,OFFSET FLAT:pd_msgmach
        call    pd_e9str
        mov     eax,[pd_ndis_mach]
        call    pd_e9hex

        ; NdisRegisterAdapter( &status, &ctx, machandle, adapterctx,
        ; &chartable, size ) - status 0 = the adapter is registered
        push    MACC_SIZE
        push    OFFSET FLAT:pd_macchar
        push    OFFSET FLAT:pd_adapter
        mov     eax,[pd_ndis_mach]
        push    eax
        push    OFFSET FLAT:pd_ndis_ctx
        push    OFFSET FLAT:pd_ndis_status
        call    nd_reg_adapter
        add     esp,24
        mov     esi,OFFSET FLAT:pd_msgreg
        call    pd_e9str
        mov     eax,[pd_ndis_status]
        call    pd_e9hex
        mov     eax,[pd_ndis_status]
        test    eax,eax
        jnz     pd_ndisfail
        mov     byte ptr [pd_ndis_ok],1
        mov     esi,OFFSET FLAT:pd_msgregok
        call    pd_e9str
        mov     eax,[pd_ndis_ctx]
        call    pd_e9hex
        jmp     pd_ndisdone
pd_ndisfail:
        ; registration failed: undo the MAC registration (NdisDeregisterMac)
        push    0
        mov     eax,[pd_ndis_mach]
        push    eax
        call    nd_dereg
        add     esp,8
        jmp     pd_ndisdone
pd_nondis:
        mov     esi,OFFSET FLAT:pd_msgnondis
        call    pd_e9str
pd_ndisdone:

        ; Arm the async tick.  From here the parked stack runs on its own,
        ; PD_TICKMS apart, with Windows never paused.
        mov     esi,OFFSET FLAT:pd_msgarm
        call    pd_e9str
        mov     eax,PD_TICKMS
        xor     edx,edx
        mov     esi,OFFSET FLAT:pd_tick
        VXDCALL S_SET_GLOBAL_TIME_OUT
        jmp     pd_done

pd_nobridge:
        mov     esi,OFFSET FLAT:pd_msgnobr
        call    pd_e9str
        jmp     pd_done
pd_gdtbig:
        mov     dword ptr [pd_pg],0             ; the bridge stays down
        mov     esi,OFFSET FLAT:pd_msggdt
        call    pd_e9str
        jmp     pd_done
pd_noalloc:
        mov     esi,OFFSET FLAT:pd_msgnoal
        call    pd_e9str
pd_done:
        popad
        clc
        ret

;---------------------------------------------------------------------
; pd_strcopy - copy the ASCIIZ string at DS:ESI to ES:EDI, at most 127
; bytes and a terminator (the SOCK_RES name field is 128).  The source
; is a fixed string, so it cannot run away, but it is bounded regardless.
;---------------------------------------------------------------------
pd_strcopy:
        push    eax
        push    ecx
        mov     ecx,127
        cld
psc_next:
        lodsb
        stosb
        or      al,al
        jz      psc_done
        dec     ecx
        jnz     psc_next
        xor     al,al
        stosb                                   ; force the terminator
psc_done:
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; pd_tramp_src..pd_tramp_end - the world-switch trampoline, copied into
; the low page and run there.  Position-independent: EBP is the page
; base on entry, every cell is reached as [ebp+disp], and the page base
; is recovered after brg_entry from the page-aligned scratch stack (so
; it survives whatever brg_entry does to EBP).  No stack or non-page
; memory is touched while the kernel's map is active.
;---------------------------------------------------------------------
pd_tramp_src:
        cli
        mov     eax,cr3
        mov     [ebp+PGO_WCR3],eax              ; save WIN386's CR3
        mov     [ebp+PGO_WESP],esp              ; save WIN386's ESP
        ; The native kernel marks its own pages read-only but runs with
        ; CR0.WP clear, so its own ring-0 stores still land (pg_protect,
        ; PAGING.INC).  WIN386 runs with WP set, so clear it for the
        ; window, or brg_entry's and the driver's writes to kernel pages
        ; fault - and an exception here, with WfW's IDT unreachable under
        ; the kernel's map, is a triple fault.
        mov     eax,cr0
        mov     [ebp+PGO_WCR0],eax
        and     eax,0FFFEFFFFh                  ; CR0.WP = 0 (bit 16)
        mov     cr0,eax
        mov     eax,[ebp+PGO_KCR3]
        mov     cr3,eax                         ; --> the kernel's identity map
        lgdt    fword ptr [ebp+PGO_LGDTR]       ; --> the low GDT copy, so segment
                                                ; loads find their descriptors
        lea     esp,[ebp+PGO_STKTOP]            ; scratch stack, in the low page
        mov     eax,[ebp+PGO_RQEAX]
        mov     edx,[ebp+PGO_RQEDX]
        mov     ebx,[ebp+PGO_RQEBX]
        mov     ecx,[ebp+PGO_RQECX]
        mov     edi,[ebp+PGO_RQEDI]
        mov     esi,[ebp+PGO_KENTRY]
        call    esi                             ; brg_entry: find + call driver
        pushfd                                  ; keep its CF before AND clears it
        mov     ebp,esp
        and     ebp,0FFFFF000h                  ; EBP = the page base again
        mov     [ebp+PGO_RET],eax
        pop     eax
        mov     [ebp+PGO_FLAGS],eax
        lgdt    fword ptr [ebp+PGO_WGDTR]       ; --> back to WIN386's GDT
        mov     eax,[ebp+PGO_WCR3]
        mov     cr3,eax                         ; --> back to WIN386's map
        mov     eax,[ebp+PGO_WCR0]
        mov     cr0,eax                         ; restore WIN386's WP
        mov     esp,[ebp+PGO_WESP]
        mov     eax,[ebp+PGO_FLAGS]
        push    eax
        popfd                                   ; live flags = the driver's (its
                                                ; CF for the caller; IF stays off,
                                                ; as it was through the window)
        mov     eax,[ebp+PGO_RET]
        ret                                     ; no sti: pd_bridge_call restores
                                                ; the caller's interrupt flag, so
                                                ; an ISR/tick caller keeps its own
pd_tramp_end:
.ERRE   (pd_tramp_end - pd_tramp_src) LE 200h, <the trampoline overruns the low page's cells>
_ITEXT  ENDS

_IDATA  SEGMENT
pd_hello        db      'PMDOS.386: Device_Init - bridge VxD loaded', 13, 10, 0
pd_msgpgd       db      'PMDOS.386: kernel CR3=', 0
pd_msgent       db      ' brg_entry=', 0
pd_msgpg        db      ' page=', 0
pd_msgcall      db      13, 10, 'PMDOS.386: self-test NIC_INFO, answer(exp 00000034)=', 0
pd_msgmac       db      ' MAC=', 0
pd_msgname      db      ' name=', 0
pd_msgnobr      db      13, 10, 'PMDOS.386: kernel has no bridge signature', 13, 10, 0
pd_msgnoal      db      13, 10, 'PMDOS.386: global V86 allocation failed', 13, 10, 0
pd_msggdt       db      13, 10, 'PMDOS.386: WIN386 GDT is too big for the low page', 13, 10, 0
pd_crlf         db      13, 10, 0
pd_msgirq       db      13, 10, 'PMDOS.386: card IRQ=', 0
pd_msgvirq      db      13, 10, 'PMDOS.386: VPICD took the card line, handle=', 0
pd_msgwkirq     db      13, 10, 'PMDOS.386: NIC_WAKE_IRQ (interrupt live), running=', 0
pd_msgwake      db      13, 10, 'PMDOS.386: NIC_WAKE, running=', 0
pd_msgkick      db      13, 10, 'PMDOS.386: BA1 name set, the tick will look up example.com once the interface is up', 13, 10, 0
pd_msgi13       db      13, 10, 'PMDOS.386: USB INT 13h hook, drive mask=', 0
pd_msgarm       db      13, 10, 'PMDOS.386: async tick armed - the parked stack now runs', 13, 10, 0
pd_resname      db      'example.com', 0
_IDATA  ENDS

        END
