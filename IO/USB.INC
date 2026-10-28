;=====================================================================
; USB.INC - the Universal Serial Bus: the controllers, the devices on
;           them, and the keyboards, mice and disks among those
;
; A PC of the late nineties has a USB controller on its south bridge
; and a BIOS that, with "USB legacy support" switched on, drives it
; from System Management Mode: the SMM code emulates an 8042 for a USB
; keyboard and an INT 13h disk for a USB stick, and traps the ports to
; do it.  That emulation is what every DOS has been living on.  It
; stops the moment anybody else drives the controller, and a USB stick
; plugged in after boot never had any at all - so a kernel that wants
; USB disks drives the controller itself, and from then on it owns the
; keyboard and the mouse on that bus as well.  This file and the four
; beside it are that:
;
;   UHCI.INC     Intel's controller (PIIX3 onwards, VIA), I/O ports and
;                a frame list of 1024 pointers
;   OHCI.INC     everybody else's (SiS, ALi, NEC, Compaq, Apple), memory
;                mapped registers and a done queue
;   USBHID.INC   boot-protocol keyboards and mice, into KEYBD.INC and
;                MOUSE.INC as if the 8042 had delivered them
;   USBMSC.INC   mass storage, bulk-only, as the block layer's drives
;                USB_DRV0 upwards
;
; This file is what the four share: finding the controllers, the
; memory their schedules live in, the device table, the transfers, and
; the state machine that enumerates whatever is plugged in.
;
; WHO RUNS WHAT, AND WHEN.  There are no threads in this kernel and a
; USB device takes a hundred milliseconds and a dozen round trips to
; set up, so nothing here ever WAITS for a device.  The timer tick
; (usb_tick, from irq_timer) looks at the root ports, takes the reports
; the keyboards and mice have sent, and moves one enumeration along one
; step - a step is one control transfer or one timed pause, and the
; next tick asks whether it finished.  The kernel needs no more than
; that because a keyboard has to be found whether or not anything asks
; for it.  A DISK, on the other hand, is only ever spoken to when the
; block layer asks for a sector, and that is the FOREGROUND: an INT 21h
; or an INT 13h, interrupts on, which polls its bulk transfer to the
; end the way ATA.INC polls a drive.  The two never share a queue: the
; tick owns the interrupt queues and, when it holds usb_ctlown, the
; control queue; the foreground owns the bulk queue and takes the
; control queue only when the tick has let go of it.
;
; WHAT THE SCHEDULE LIVES IN.  Everything the controller reads - frame
; lists, queue heads, transfer descriptors, the HCCA - has to be at a
; PHYSICAL address, and the identity map (PAGING.INC) makes every
; address of ours one, with the two exceptions ATA.INC lists.  A UHCI
; frame list is 4K and 4K-aligned, which PMIO's 64K image cannot spare,
; so PMDOS buys a block out of the arena once CONFIG.SYS has said
; whether USB is wanted (USF_START) and PMIO carves it up: the frame
; lists first, then the OHCI communication areas, then a pool of
; 32-byte elements that are TDs, QHs and EDs by turns, and the scratch
; buffers at the top.  Nothing else is ever allocated.
;
; THE BIOS IS ASKED TO LET GO FIRST.  On UHCI that is the LEGSUP
; register in configuration space, which routes port 60h and 64h to
; SMI; on OHCI the InterruptRouting bit, which the BIOS clears when
; asked through OwnershipChangeRequest; on EHCI the extended
; capability's BIOS-owned semaphore, and then CONFIGFLAG cleared so
; that a high-speed port hands its device to the UHCI or OHCI beside
; it, where this kernel can reach it.  A park (Windows, RMDOS) gives
; the controllers back the way they were found, registers and all,
; and takes them again on the way back - whether the BIOS's SMM code
; picks up where it left off is the BIOS's affair and could not be
; tested here: no emulator has one.
;
; Included by IOPM.ASM after BLOCK.INC; UHCI.INC, OHCI.INC, USBHID.INC
; and USBMSC.INC follow it.
;=====================================================================

;--- the pool: one element is a TD, a QH or an ED -------------------
EL_LEN          EQU     32
EL_HW0          EQU     00h             ; the four dwords the controller reads
EL_HW1          EQU     04h
EL_HW2          EQU     08h
EL_HW3          EQU     0Ch
EL_NEXT         EQU     10h             ; dd - the next element of the chain
                                        ;      (software's order), or of the
                                        ;      free list
EL_BUF          EQU     14h             ; dd - where its data goes
EL_LEN16        EQU     18h             ; dw - bytes asked for
EL_FLAGS        EQU     1Ah             ; db - ELF_*
EL_TOG          EQU     1Bh             ; db - the toggle it was built with
EL_PREV         EQU     1Ch             ; dd - the element before it (rings)

ELF_IN          EQU     01h             ; data comes from the device
ELF_LAST        EQU     02h             ; the last of its stage
ELF_SETUP       EQU     04h
ELF_SKIP        EQU     08h             ; never run: a short packet ended its
                                        ; stage before it (usb_chainpoll)
ELF_LS          EQU     10h             ; the device is low speed
ELF_DUMMY       EQU     20h             ; OHCI: the ring's placeholder tail
ELF_FIRST       EQU     40h             ; the first of its stage
ELF_BULK        EQU     80h             ; a bulk stage: the toggle is the
                                        ; endpoint's, carried in the ED

;--- the controller table -------------------------------------------
HC_KIND         EQU     USBH_KIND
HC_NPORTS       EQU     USBH_NPORTS
HC_FLAGS        EQU     USBH_FLAGS
HC_IRQ          EQU     USBH_IRQ
HC_PCI          EQU     USBH_PCI
HC_BASE         EQU     USBH_BASE
HC_FL           EQU     0Ch             ; dd - frame list (UHCI), HCCA (OHCI)
HC_CTLQ         EQU     10h             ; dd - the control QH / ED
HC_BLKQ         EQU     14h             ; dd - the bulk QH / ED
HC_INTQ         EQU     18h             ; dd - UHCI: the skeleton QH the frame
                                        ;      list points at
HC_INTHEAD      EQU     1Ch             ; dd - the first interrupt QH / ED, 0
HC_SAVE         EQU     20h             ; 16 bytes - the registers as found
HC_CTLDUMMY     EQU     30h             ; dd - OHCI: the control ED's tail
HC_BLKDUMMY     EQU     34h             ; dd - ...and the bulk ED's
HC_SMIWAS       EQU     38h             ; dd - EHCI: USBLEGCTLSTS as found
HC_EECP         EQU     3Ch             ; db - EHCI: where its legacy words are
HC_LEN          EQU     40h

;--- the disk units, USBMSC.INC's -----------------------------------
UN_DEV          EQU     00h             ; db - the device slot, FFh for none
UN_FLAGS        EQU     01h             ; db - USBUF_*
UN_GEN          EQU     02h             ; db - counts up at every attach
UN_LUN          EQU     03h             ; db
UN_SECS         EQU     04h             ; dd - capacity, 0 until asked
UN_ALIAS        EQU     08h             ; db - the BIOS drive this unit took
                                        ;      the place of (80h/81h), or 0
UN_BPSLOG       EQU     09h             ; db - log2 of the block size
UN_SENSE        EQU     0Ah             ; db - the last sense key
UN_ASC          EQU     0Bh             ; db - ...and its code
UN_MODEL        EQU     0Ch             ; 24 bytes - INQUIRY's vendor and
                                        ;      product, space padded
UN_TAG          EQU     24h             ; dd - the next CBW tag
UN_LEN          EQU     28h

;--- what the scratch area at the top of the block holds ------------
SCR_SETUP       EQU     0000h           ; 8 bytes - the setup packet in flight
SCR_LED         EQU     0008h           ; 8 bytes - a keyboard's lights
SCR_DESC        EQU     0010h           ; 512 - descriptors, hub status
SCR_CBW         EQU     0210h           ; 32
SCR_CSW         EQU     0230h           ; 32
SCR_CDB         EQU     0250h           ; 16 - the command block being sent
SCR_INQ         EQU     0260h           ; 64 - INQUIRY, READ CAPACITY, sense
SCR_SAVE0       EQU     0300h           ; 512 - BIOS drive 80h's sector 0
SCR_SAVE1       EQU     0500h           ; 512 - ...and 81h's (usb_adopt)
SCR_RINGS       EQU     0700h           ; USB_MAXDEV * RING_N * 8 = 1K
SCR_SECT        EQU     0B00h           ; 512 - a sector for the probes
SCR_BOUNCE      EQU     1000h           ; 4K - for a buffer the bus cannot see
SCR_LEN         EQU     2000h

RING_N          EQU     16              ; interrupt descriptors a HID device
                                        ; keeps queued: 16 reports is 128ms
                                        ; of a mouse between two ticks
RING_BUF        EQU     8               ; bytes each

;--- jobs: what the tick has a control transfer out for -------------
JOB_NONE        EQU     0
JOB_ENUM        EQU     1               ; a step of usb_enumdev's enumeration
JOB_HUB         EQU     2               ; a hub's port being asked about
JOB_LED         EQU     3               ; a keyboard's lights

;--- the enumeration substates (USBD_SUB) ---------------------------
ES_DEBOUNCE     EQU     1               ; connected: let the contacts settle
ES_RESET        EQU     2               ; reset asserted at USBD_TIME
ES_RESETEND     EQU     3               ; ...released, port being enabled
ES_RECOVER      EQU     4               ; enabled: the device's 10ms
ES_GETDESC8     EQU     5               ; GET_DESCRIPTOR(device), 8 bytes
ES_SETADDR      EQU     6               ; SET_ADDRESS
ES_ADDRWAIT     EQU     7               ; ...and its 2ms
ES_GETDEV       EQU     8               ; GET_DESCRIPTOR(device), all of it
ES_GETSTR       EQU     9               ; GET_DESCRIPTOR(string iProduct)
ES_GETCFG9      EQU     10              ; GET_DESCRIPTOR(configuration), 9
ES_GETCFG       EQU     11              ; ...and the whole of it
ES_SETCFG       EQU     12              ; SET_CONFIGURATION
ES_CLASS1       EQU     13              ; the class's first request
ES_CLASS2       EQU     14              ; ...and its second
ES_HUBPWR       EQU     15              ; a hub: PORT_POWER on port USBD_HPORT
ES_HUBWAIT      EQU     16              ; ...and the power-good time
ES_HRESET       EQU     17              ; a hub's child: PORT_RESET sent
ES_HRESETPOLL   EQU     18              ; ...GET_PORT_STATUS until it is done
ES_HRESETACK    EQU     19              ; ...C_PORT_RESET cleared

;--- the hub job's steps (usb_hubstep) -------------------------------
HJ_STATUS       EQU     1               ; GET_PORT_STATUS out
HJ_CLEARCSC     EQU     2               ; CLEAR_FEATURE(C_PORT_CONNECTION) out
HJ_CLEARPESC    EQU     3               ; CLEAR_FEATURE(C_PORT_ENABLE) out

;--- USB itself -------------------------------------------------------
PID_SETUP       EQU     2Dh
PID_IN          EQU     69h
PID_OUT         EQU     0E1h

UREQ_GETSTATUS  EQU     00h
UREQ_CLRFEAT    EQU     01h
UREQ_SETFEAT    EQU     03h
UREQ_SETADDR    EQU     05h
UREQ_GETDESC    EQU     06h
UREQ_SETCFG     EQU     09h
UREQ_SETIDLE    EQU     0Ah             ; HID
UREQ_SETPROTO   EQU     0Bh             ; HID
UREQ_SETREPORT  EQU     09h             ; HID
UREQ_GETMAXLUN  EQU     0FEh            ; mass storage
UREQ_BOTRESET   EQU     0FFh

UDT_DEVICE      EQU     1
UDT_CONFIG      EQU     2
UDT_STRING      EQU     3
UDT_INTERFACE   EQU     4
UDT_ENDPOINT    EQU     5
UDT_HUB         EQU     29h

UFEAT_EPHALT    EQU     0
HUBF_PORTENABLE EQU     1
HUBF_PORTRESET  EQU     4
HUBF_PORTPOWER  EQU     8
HUBF_C_CONN     EQU     16
HUBF_C_ENABLE   EQU     17
HUBF_C_RESET    EQU     20

; a hub's port status word, and the change word above it
HPS_CONNECT     EQU     0001h
HPS_ENABLE      EQU     0002h
HPS_RESET       EQU     0010h
HPS_POWER       EQU     0100h
HPS_LOWSPEED    EQU     0200h
HPC_CONNECT     EQU     0001h           ; C_PORT_CONNECTION
HPC_ENABLE      EQU     0002h
HPC_RESET       EQU     0010h

; what hc_portst answers
PST_CONNECT     EQU     01h
PST_CHANGE      EQU     02h             ; connect status changed since acked
PST_ENABLED     EQU     04h
PST_LOWSPEED    EQU     08h
PST_RESETDONE   EQU     10h             ; OHCI: the reset it was told to do
                                        ; has finished

; what a transfer poll answers in AL, and in AH when it is 2
XS_PENDING      EQU     0
XS_DONE         EQU     1
XS_ERROR        EQU     2
USBE_STALL      EQU     1
USBE_TIMEOUT    EQU     2               ; nothing answered, or not in time
USBE_CRC        EQU     3
USBE_BABBLE     EQU     4
USBE_GONE       EQU     5               ; the device was unplugged
USBE_NOMEM      EQU     6               ; the pool ran dry
USBE_OTHER      EQU     7

CTL_TICKS       EQU     19              ; a control transfer has a second
BULK_TICKS      EQU     92              ; a bulk one five
ENUM_TRIES      EQU     3               ; a step is tried this often
XFER_MAX        EQU     4000h           ; bytes one bulk chain carries: 16K is
                                        ; 256 UHCI descriptors of 64 bytes

;=====================================================================
; usb_init - once, from pm_entry after pci_init: count the controllers.
;            Nothing is touched; the BIOS goes on driving them until
;            PMDOS says otherwise (usb_start).
;=====================================================================
usb_init:
        pushad
        call    usbm_init
        mov     byte ptr [usb_hcn],0
        mov     byte ptr [usb_ehcin],0
        mov     byte ptr [usb_started],0
        cmp     byte ptr [pci_ok],0
        je      ui_out
        xor     ebp,ebp                 ; EBP = which match of the class
ui_next:
        mov     esi,ebp
        mov     edi,PCT_CLASS
        mov     edx,0FFFF0000h          ; class and subclass: serial bus, USB
        mov     ecx,0C030000h
        call    pcx_match               ; -> EBX = address, ESI -> the entry
        jc      ui_out
        mov     eax,dword ptr [esi+PCT_CLASS]
        shr     eax,8
        and     al,0F0h                 ; the programming interface: 00 UHCI,
        mov     cl,USBK_UHCI            ; 10 OHCI, 20 EHCI, 30 xHCI
        or      al,al
        jz      ui_have
        mov     cl,USBK_OHCI
        cmp     al,10h
        je      ui_have
        mov     cl,USBK_EHCI
        cmp     al,20h
        jne     ui_step                 ; an xHCI, or a stranger: left alone
ui_have:
        movzx   edi,byte ptr [usb_hcn]
        cmp     edi,USB_MAXHC
        jae     ui_step
        imul    edi,HC_LEN
        add     edi,OFFSET usb_hcs
        mov     [edi+HC_KIND],cl
        mov     dword ptr [edi+HC_PCI],ebx
        mov     byte ptr [edi+HC_FLAGS],0
        mov     al,PCIR_INTR
        call    pci_read
        mov     [edi+HC_IRQ],al
        inc     byte ptr [usb_hcn]
        cmp     cl,USBK_EHCI
        jne     ui_step
        inc     byte ptr [usb_ehcin]
ui_step:
        inc     ebp
        jmp     ui_next
ui_out:
        popad
        ret

;=====================================================================
; usb_svc - IOS_USB.  AL = a USF_* function.
;=====================================================================
usb_svc:
        cmp     al,USF_COUNT
        je      usb_fcount
        cmp     al,USF_START
        je      usb_start
        cmp     al,USF_UNIT
        je      usbm_funit
        cmp     al,USF_DEVICE
        je      usb_fdevice
        cmp     al,USF_HC
        je      usb_fhc
        cmp     al,USF_RESCAN
        je      usb_frescan
        cmp     al,USF_PROBE
        je      usbm_fprobe
        cmp     al,USF_KEEPALIVE
        je      usb_fkalive
        stc
        ret

;--- USF_KEEPALIVE: CONFIG.SYS asked USB=ON,KEEPALIVE, so keep the
;--- controllers owned across the park for the WfW bridge --------------
usb_fkalive:
        mov     byte ptr [usb_kalive],1
        clc
        ret

;--- USF_COUNT: how many controllers there are to drive -------------
usb_fcount:
        movzx   ecx,byte ptr [usb_hcn]
        movzx   edx,byte ptr [usb_ehcin]
        sub     ecx,edx
        clc
        ret

;--- USF_DEVICE: slot DL, copied to EDI ------------------------------
usb_fdevice:
        push    esi
        push    ecx
        movzx   ecx,dl
        cmp     ecx,USB_MAXDEV
        jae     ufd_no
        imul    esi,ecx,USBD_LEN
        add     esi,OFFSET usb_devs
        cmp     byte ptr [esi+USBD_STATE],USBS_FREE
        je      ufd_no
        push    edi
        mov     ecx,USBD_LEN / 4
        rep     movsd
        pop     edi
        pop     ecx
        pop     esi
        mov     ecx,USB_MAXDEV
        clc
        ret
ufd_no:
        pop     ecx
        pop     esi
        mov     ecx,USB_MAXDEV
        stc
        ret

;--- USF_HC: controller DL, its first USBH_LEN bytes to EDI ----------
usb_fhc:
        push    esi
        push    ecx
        movzx   ecx,dl
        cmp     cl,[usb_hcn]
        jae     ufh_no
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        push    edi
        mov     ecx,USBH_LEN / 4
        rep     movsd
        pop     edi
        pop     ecx
        pop     esi
        movzx   ecx,byte ptr [usb_hcn]
        clc
        ret
ufh_no:
        pop     ecx
        pop     esi
        movzx   ecx,byte ptr [usb_hcn]
        stc
        ret

;--- USF_RESCAN: every change bit forgotten and every port looked at -
usb_frescan:
        cmp     byte ptr [usb_started],0
        je      ufr_out
        pushfd
        cli
        mov     byte ptr [usb_forcescan],1
        popfd
ufr_out:
        clc
        ret

;=====================================================================
; usb_start - USF_START.  EBX = the block PMDOS bought, 4K aligned,
;             USB_MEMNEED bytes.  CF if there is no controller to take.
;
; The controllers are taken from the BIOS, the devices that are
; plugged in are enumerated - by the tick, which this waits for with
; interrupts on - and the disks among them are asked what they are, so
; that PMDOS can give them letters and say so before the prompt.
;=====================================================================
usb_start:
        pushad
IFDEF HANG_E9
        mov     al,'S'
        out     0E9h,al
ENDIF
        cmp     byte ptr [usb_started],0
        jne     ust_done                ; twice is once
        mov     al,[usb_hcn]
        sub     al,[usb_ehcin]
        jz      ust_none

        ; the block: frame lists, then HCCAs, then the pool
        mov     dword ptr [usb_mem],ebx
        lea     eax,[ebx + USB_MEMNEED - SCR_LEN]
        mov     dword ptr [usb_scr],eax
        call    usb_carve

        ; sector 0 of the BIOS's hard disks, before the BIOS stops
        ; being able to read them - see usb_adopt
        call    usb_savebios

        ; the EHCI controllers first: their ports have to come down
        ; to the companions before the companions see anything
        xor     ecx,ecx
ust_ehci:
        cmp     cl,[usb_hcn]
        jae     ust_take
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        cmp     byte ptr [esi+HC_KIND],USBK_EHCI
        jne     ust_enext
        call    ehci_handdown
ust_enext:
        inc     ecx
        jmp     ust_ehci

ust_take:
        xor     ecx,ecx
ust_hc:
        cmp     cl,[usb_hcn]
        jae     ust_taken
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        cmp     byte ptr [esi+HC_KIND],USBK_EHCI
        je      ust_hnext
        call    hc_init                 ; CF: left alone
        jc      ust_hnext
        or      byte ptr [esi+HC_FLAGS],USBHF_OWNED
ust_hnext:
        inc     ecx
        jmp     ust_hc
ust_taken:
        mov     byte ptr [usb_forcescan],1
        mov     byte ptr [usb_started],1
IFDEF HANG_E9
        mov     al,'s'
        out     0E9h,al
ENDIF

        ; and now the tick does the work: wait until it has had nothing
        ; to do for a while, or for five seconds
        call    usb_settle
IFDEF HANG_E9
        mov     al,'T'
        out     0E9h,al
ENDIF

        ; the disks: asked what they are now, so that letters can be
        ; announced, and so that a stick the BIOS booted from can be
        ; recognised as the disk C: already stands on
        call    usbm_probeall
IFDEF HANG_E9
        mov     al,'P'
        out     0E9h,al
ENDIF
        call    usb_adopt
IFDEF HANG_E9
        mov     al,'A'
        out     0E9h,al
ENDIF
ust_done:
        popad
        clc
        ret
ust_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; usb_settle - with interrupts on, wait for the enumeration to go
;              quiet: no device being set up for eight ticks running,
;              every hub's ports looked at once, and a ceiling of five
;              seconds whatever is happening.
;---------------------------------------------------------------------
usb_settle:
        pushad
        sti
        mov     ebp,dword ptr ds:[BDA_TICKS]    ; when this began
        xor     edi,edi                         ; quiet ticks so far
        mov     ebx,ebp
usl_lp:
IFDEF HANG_E9
        ; what the wait is seeing: the clock, the timer's divisor and
        ; accumulator, the device being set up and the job out
        inc     dword ptr [usl_spins]
        test    dword ptr [usl_spins],0FFFFFh
        jnz     usl_quiet
        push    esi
        mov     esi,OFFSET msgUsl
        call    e9_str
        mov     eax,dword ptr ds:[BDA_TICKS]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [vt_div]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [vt_acc]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        movzx   eax,byte ptr [usb_enumdev]
        call    e9_hex8
        mov     al,' '
        out     0E9h,al
        movzx   eax,byte ptr [usb_job]
        call    e9_hex8
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
        pop     esi
usl_quiet:
ENDIF
        mov     eax,dword ptr ds:[BDA_TICKS]
        cmp     eax,ebx
        je      usl_lp                          ; not a new tick yet
        mov     ebx,eax
        sub     eax,ebp
        cmp     eax,91                          ; five seconds
        jae     usl_out
        cmp     byte ptr [usb_enumdev],0FFh
        jne     usl_busy
        cmp     byte ptr [usb_job],JOB_ENUM
        je      usl_busy
        call    usb_hubsswept                   ; CF: a hub not yet looked at
        jc      usl_busy
        inc     edi
        cmp     edi,8
        jb      usl_lp
usl_out:
        popad
        ret
usl_busy:
        xor     edi,edi
        jmp     usl_lp

;---------------------------------------------------------------------
; usb_hubsswept - CF if a hub in service has not had every port asked
;                 about since it was configured.
;---------------------------------------------------------------------
usb_hubsswept:
        push    ebx
        push    ecx
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
uhs_lp:
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     uhs_next
        cmp     byte ptr [ebx+USBD_CLASS],USBC_HUB
        jne     uhs_next
        test    byte ptr [ebx+USBD_FLAGS],USBDF_SWEPT
        jz      uhs_no
uhs_next:
        add     ebx,USBD_LEN
        loop    uhs_lp
        pop     ecx
        pop     ebx
        clc
        ret
uhs_no:
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; usb_carve - the block at usb_mem into frame lists, HCCAs, the pool
;             and the scratch area.
;---------------------------------------------------------------------
usb_carve:
        pushad
        mov     edi,dword ptr [usb_mem]
        ; the whole block to zero: a frame list of zeros is a frame
        ; list of pointers to address 0, which is why every entry is
        ; written below before the controller is shown it
        push    edi
        mov     ecx,USB_MEMNEED / 4
        xor     eax,eax
        rep     stosd
        pop     edi

        ; frame lists, 4K each, for the UHCI controllers
        xor     ecx,ecx
uc_fl:
        cmp     cl,[usb_hcn]
        jae     uc_hcca
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        jne     uc_flnext
        mov     dword ptr [esi+HC_FL],edi
        add     edi,1000h
uc_flnext:
        inc     ecx
        jmp     uc_fl

        ; the HCCAs, 256 bytes each, for the OHCI ones
uc_hcca:
        xor     ecx,ecx
uc_hc:
        cmp     cl,[usb_hcn]
        jae     uc_pool
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        cmp     byte ptr [esi+HC_KIND],USBK_OHCI
        jne     uc_hcnext
        mov     dword ptr [esi+HC_FL],edi
        add     edi,100h
uc_hcnext:
        inc     ecx
        jmp     uc_hc

        ; the pool: from here to the scratch area, as a free list
uc_pool:
        add     edi,EL_LEN - 1
        and     edi,NOT (EL_LEN - 1)
        mov     dword ptr [usb_poolfree],edi
        mov     dword ptr [usb_nelem],0
        mov     edx,dword ptr [usb_scr]
uc_el:
        lea     eax,[edi+EL_LEN]
        cmp     eax,edx
        ja      uc_last
        mov     dword ptr [edi+EL_NEXT],eax
        mov     edi,eax
        inc     dword ptr [usb_nelem]
        jmp     uc_el
uc_last:
        ; the one that did not fit is not on the list; the last one
        ; that did ends it
        mov     eax,dword ptr [usb_poolfree]
        mov     ecx,dword ptr [usb_nelem]
        dec     ecx
        imul    ecx,EL_LEN
        add     eax,ecx
        mov     dword ptr [eax+EL_NEXT],0
        popad
        ret

;---------------------------------------------------------------------
; el_alloc - EAX = a cleared element off the free list.  CF if none.
; el_free  - EAX = an element, back on it.
;
; Both with interrupts off for the two instructions that matter: the
; tick allocates for the HID rings and the foreground for the bulk
; chains, and a list with two heads is a list that loses elements.
;---------------------------------------------------------------------
el_alloc:
        push    ebx
        pushfd
        cli
        mov     eax,dword ptr [usb_poolfree]
        or      eax,eax
        jz      ela_none
        mov     ebx,dword ptr [eax+EL_NEXT]
        mov     dword ptr [usb_poolfree],ebx
        popfd
        push    ecx
        push    edi
        mov     edi,eax
        mov     ecx,EL_LEN / 4
        push    eax
        xor     eax,eax
        rep     stosd
        pop     eax
        pop     edi
        pop     ecx
        pop     ebx
        clc
        ret
ela_none:
        popfd
        pop     ebx
        stc
        ret

el_free:
        push    ebx
        pushfd
        cli
        mov     ebx,dword ptr [usb_poolfree]
        mov     dword ptr [eax+EL_NEXT],ebx
        mov     dword ptr [usb_poolfree],eax
        popfd
        pop     ebx
        ret

;---------------------------------------------------------------------
; el_freechain - EAX = the first element of a chain linked by EL_NEXT,
;                every one of them freed.  EAX comes back 0.
;---------------------------------------------------------------------
el_freechain:
        push    ebx
efc_lp:
        or      eax,eax
        jz      efc_out
        mov     ebx,dword ptr [eax+EL_NEXT]
        call    el_free
        mov     eax,ebx
        jmp     efc_lp
efc_out:
        pop     ebx
        ret

;=====================================================================
; THE CONTROLLER INTERFACE.  Every routine here takes ESI -> the
; controller's row and goes to UHCI.INC or OHCI.INC by its kind.
;
;   hc_init          take it from the BIOS and start the schedule.  CF
;                    if it would not
;   hc_handback      stop it and put the registers back as found
;   hc_halt          stop it, for a restart of the machine
;   hc_portst        CL = port (from 1) -> AL = PST_*
;   hc_portack       CL -> the change bits written off
;   hc_portreset     CL -> reset asserted
;   hc_portenable    CL -> reset released and the port enabled (UHCI)
;   hc_mktd          one descriptor: EBX -> device, DL = PID, DH =
;                    endpoint, EDI = buffer, ECX = bytes, AL = toggle,
;                    AH = ELF_* -> EAX = the element.  CF: pool empty
;   hc_tdmax         the most one descriptor carries from EDI with
;                    ECX left and DX the packet size -> ECX
;   hc_link          EAX -> EDX: the controller follows one to the next
;   hc_term          EAX ends its chain
;   hc_run           EDI = queue, EAX = first, EBX -> device, CL =
;                    endpoint (bit 7 for IN), CH = the toggle to start
;                    a bulk transfer with (OHCI carries it in the ED),
;                    DX = packet size: the chain handed to the
;                    controller
;   hc_stop          EDI = queue: nothing more is executed from it
;   hc_skipto        EDI = queue, EAX = a descriptor of the chain on it:
;                    the controller goes on from that one (UHCI, after
;                    a short packet; OHCI needs nothing)
;   hc_tdstat        EAX = a descriptor -> AL = XS_*, AH = USBE_*, ECX =
;                    bytes moved
;   hc_qtoggle       EDI = a queue -> AL = its toggle carry (OHCI; UHCI
;                    answers from the last descriptor, see usb_blktog)
;   hc_intrq         EBX -> device -> EDI = a new interrupt queue, in
;                    the periodic schedule.  CF: pool empty
;   hc_intrdel       EDI = that queue, taken out of the schedule
;   hc_ringadd       EDI = queue, EAX = a descriptor for it (EL_BUF and
;                    EL_LEN16 set): queued behind the others
;   hc_ringst        EAX = a ring descriptor -> AL = XS_*, ECX = bytes
;   hc_rearm         EAX = a ring descriptor that has been read: active
;                    again at the back of the ring
;   hc_ack           housekeeping once a tick (OHCI's done queue)
;=====================================================================
hc_init:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_init
        jmp     oh_init
hc_handback:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_handback
        jmp     oh_handback
hc_halt:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_halt
        jmp     oh_halt
hc_portst:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_portst
        jmp     oh_portst
hc_portack:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_portack
        jmp     oh_portack
hc_portreset:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_portreset
        jmp     oh_portreset
hc_portenable:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_portenable
        jmp     oh_portenable
hc_mktd:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_mktd
        jmp     oh_mktd
hc_tdmax:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_tdmax
        jmp     oh_tdmax
hc_link:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_link
        jmp     oh_link
hc_term:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_term
        jmp     oh_term
hc_run:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_run
        jmp     oh_run
hc_stop:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_stop
        jmp     oh_stop
hc_skipto:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_skipto
        jmp     oh_skipto
hc_tdstat:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_tdstat
        jmp     oh_tdstat
hc_qtoggle:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_qtoggle
        jmp     oh_qtoggle
hc_intrq:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_intrq
        jmp     oh_intrq
hc_intrdel:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_intrdel
        jmp     oh_intrdel
hc_ringadd:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_ringadd
        jmp     oh_ringadd
hc_ringst:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_ringst
        jmp     oh_ringst
hc_rearm:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_rearm
        jmp     oh_rearm
hc_ack:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      uh_ack
        jmp     oh_ack

;---------------------------------------------------------------------
; dev_hc - EBX -> a device -> ESI -> its controller's row.
; dev_row - AL = a slot -> EBX -> its row.
; dev_slot - EBX -> a row -> AL = its slot.
;---------------------------------------------------------------------
dev_hc:
        movzx   esi,byte ptr [ebx+USBD_HC]
        imul    esi,HC_LEN
        add     esi,OFFSET usb_hcs
        ret
dev_row:
        movzx   ebx,al
        imul    ebx,USBD_LEN
        add     ebx,OFFSET usb_devs
        ret
dev_slot:
        push    ebx
        sub     ebx,OFFSET usb_devs
        push    edx
        push    eax
        mov     eax,ebx
        xor     edx,edx
        mov     ebx,USBD_LEN
        div     ebx
        mov     ebx,eax
        pop     eax
        pop     edx
        mov     al,bl
        pop     ebx
        ret

;---------------------------------------------------------------------
; usb_udelay - ECX microseconds, near enough: a read of port 80h is an
;              ISA cycle of about a microsecond on anything.  For the
;              few waits the hardware wants in the middle of a tick.
;---------------------------------------------------------------------
usb_udelay:
        push    eax
        push    ecx
uud_lp:
        in      al,80h
        loop    uud_lp
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; usb_waitticks - ECX clock ticks, with interrupts on.  Foreground only.
;---------------------------------------------------------------------
usb_waitticks:
        push    eax
        push    ebx
        sti
        mov     ebx,dword ptr ds:[BDA_TICKS]
uwt_lp:
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,ebx
        jae     uwt_pos
        add     eax,TICKS_PER_DAY
uwt_pos:
        cmp     eax,ecx
        jb      uwt_lp
        pop     ebx
        pop     eax
        ret

;=====================================================================
; TRANSFERS.  A control transfer is three stages of descriptors in a
; chain - SETUP, the data, the status - and a bulk one is one stage;
; the chain is handed to the controller's queue, and polled.  One of
; each kind at a time, in the two sets of statics below.
;=====================================================================

;---------------------------------------------------------------------
; usb_mkstage - a stage of descriptors.  EBX -> device, DL = PID, DH =
;               endpoint, EDI = buffer, ECX = bytes (0 for one empty
;               packet), AL = the first toggle, AH = ELF_*, and the
;               endpoint's packet size in usb_mkps.
;
;               -> EAX = first element, EDX = last, CL = the toggle
;               after the stage.  CF and nothing allocated if the pool
;               ran out.
;
; Every register is spoken for, so the loop keeps its counts in the
; usb_mk* statics: one stage is built at a time.
;---------------------------------------------------------------------
usb_mkstage:
        push    ebp
        push    esi
        push    edi
        push    ecx
        push    ebx
        mov     dword ptr [usb_mkfirst],0
        mov     dword ptr [usb_mklast],0
        mov     dword ptr [usb_mkleft],ecx
        mov     dword ptr [usb_mkbuf],edi
        mov     byte ptr [usb_mktog],al
        or      ah,ELF_FIRST            ; the first descriptor built says so
        mov     byte ptr [usb_mkflags],ah
        mov     word ptr [usb_mkpid],dx
        call    dev_hc                  ; ESI -> the controller
        cmp     word ptr [usb_mkps],0
        je      ums_fail                ; no packet size, no packets
        or      ecx,ecx
        jnz     ums_lp
        ; one zero-length packet
        mov     dx,word ptr [usb_mkpid]
        mov     al,byte ptr [usb_mktog]
        mov     ah,byte ptr [usb_mkflags]
        or      ah,ELF_LAST
        xor     ecx,ecx
        call    hc_mktd
        jc      ums_fail
        mov     dword ptr [usb_mkfirst],eax
        mov     dword ptr [usb_mklast],eax
        xor     byte ptr [usb_mktog],1
        jmp     ums_done
ums_lp:
        mov     ecx,dword ptr [usb_mkleft]
        or      ecx,ecx
        jz      ums_done
        mov     edi,dword ptr [usb_mkbuf]
        mov     dx,word ptr [usb_mkps]
        call    hc_tdmax                ; -> ECX = this descriptor's share
        mov     ebp,ecx                 ; EBP = it
        mov     dx,word ptr [usb_mkpid]
        mov     al,byte ptr [usb_mktog]
        mov     ah,byte ptr [usb_mkflags]
        cmp     ecx,dword ptr [usb_mkleft]
        jne     ums_notlast
        or      ah,ELF_LAST
ums_notlast:
        call    hc_mktd                 ; -> EAX
        jc      ums_fail
        and     byte ptr [usb_mkflags],NOT ELF_FIRST
        ; chain it behind the last
        cmp     dword ptr [usb_mkfirst],0
        jne     ums_chain
        mov     dword ptr [usb_mkfirst],eax
        jmp     ums_chained
ums_chain:
        mov     edx,eax                 ; EDX = the new one
        mov     eax,dword ptr [usb_mklast]
        mov     dword ptr [eax+EL_NEXT],edx
        call    hc_link                 ; EAX -> EDX
        mov     eax,edx
ums_chained:
        mov     dword ptr [usb_mklast],eax
        ; the toggle flips once a packet: a descriptor is one packet
        ; on UHCI and several on OHCI, where the ED carries the toggle
        ; on and the explicit one is the first descriptor's alone
        mov     eax,ebp
        movzx   ecx,word ptr [usb_mkps]
        add     eax,ecx
        dec     eax
        xor     edx,edx
        div     ecx                     ; EAX = packets in this descriptor
        test     al,1
        jz      ums_tsame
        xor     byte ptr [usb_mktog],1
ums_tsame:
        add     dword ptr [usb_mkbuf],ebp
        sub     dword ptr [usb_mkleft],ebp
        jmp     ums_lp
ums_done:
        mov     eax,dword ptr [usb_mkfirst]
        mov     edx,dword ptr [usb_mklast]
        mov     cl,byte ptr [usb_mktog]
        pop     ebx
        add     esp,4                   ; the count, consumed
        pop     edi
        pop     esi
        pop     ebp
        clc
        ret
ums_fail:
        mov     eax,dword ptr [usb_mkfirst]
        call    el_freechain
        pop     ebx
        add     esp,4
        pop     edi
        pop     esi
        pop     ebp
        stc
        ret

;---------------------------------------------------------------------
; usb_ctl_start - a control transfer.  EBX -> device, ESI -> the 8-byte
;                 setup packet, EDI -> the data (ignored when there is
;                 none), ECX = its length.  CF with AH = USBE_* if it
;                 could not be built.  The caller holds the control
;                 queue (usb_ctlown).
;---------------------------------------------------------------------
usb_ctl_start:
        pushad
        ; the setup packet goes into scratch: the controller reads it
        ; from there, and the caller's copy may be on a stack
        push    edi
        push    ecx
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_SETUP
        mov     ecx,2
        rep     movsd
        pop     ecx
        pop     edi
        mov     [usb_cxdev],ebx
        mov     dword ptr [usb_cxlen],ecx
        mov     dword ptr [usb_cxbuf],edi
        mov     dword ptr [usb_cxfirst],0
        mov     dword ptr [usb_cxlast],0
        movzx   eax,byte ptr [ebx+USBD_MAXP0]
        mov     word ptr [usb_mkps],ax
        mov     esi,dword ptr [usb_scr]
        mov     al,byte ptr [esi+SCR_SETUP]     ; bmRequestType: bit 7 is IN
        mov     byte ptr [usb_cxin],al
        call    dev_hc                  ; ESI -> the controller: hc_link and
                                        ; hc_term below dispatch on it

        ; SETUP, DATA0, eight bytes
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_SETUP
        mov     ecx,8
        mov     dl,PID_SETUP
        xor     dh,dh
        xor     al,al
        mov     ah,ELF_SETUP
        call    usb_mkstage             ; -> EAX first, EDX last
        jc      ucs_nomem
        mov     dword ptr [usb_cxfirst],eax
        mov     ebp,edx                 ; EBP = the last element so far

        ; the data stage, DATA1 first, if there is one
        mov     ecx,dword ptr [usb_cxlen]
        or      ecx,ecx
        jz      ucs_status
        mov     edi,dword ptr [usb_cxbuf]
        mov     dl,PID_OUT
        xor     ah,ah
        test    byte ptr [usb_cxin],80h
        jz      ucs_dout
        mov     dl,PID_IN
        mov     ah,ELF_IN
ucs_dout:
        xor     dh,dh
        mov     al,1
        call    usb_mkstage
        jc      ucs_nomem
        call    ucs_append              ; behind EBP; EBP = its last

ucs_status:
        ; the status stage: the other way, empty, DATA1 - and IN when
        ; there was no data stage at all
        mov     dl,PID_IN
        mov     ah,ELF_IN
        cmp     dword ptr [usb_cxlen],0
        je      ucs_sdir
        test    byte ptr [usb_cxin],80h
        jz      ucs_sdir
        mov     dl,PID_OUT
        xor     ah,ah
ucs_sdir:
        xor     dh,dh
        xor     ecx,ecx
        mov     al,1
        call    usb_mkstage
        jc      ucs_nomem
        call    ucs_append
        mov     dword ptr [usb_cxlast],ebp
        mov     eax,ebp
        call    hc_term

        ; and off it goes
        mov     ebx,[usb_cxdev]
        call    dev_hc
        mov     edi,dword ptr [esi+HC_CTLQ]
        mov     eax,dword ptr [usb_cxfirst]
        xor     ecx,ecx                 ; endpoint 0, toggle 0
        mov     dx,word ptr [usb_mkps]
        call    hc_run
IFDEF HANG_E9
        call    usb_trace_run
ENDIF
        mov     eax,dword ptr ds:[BDA_TICKS]
        mov     dword ptr [usb_cxtime],eax
        mov     byte ptr [usb_cxbusy],1
        popad
        clc
        ret

;--- ucs_append - the stage EAX..EDX goes on the chain behind EBP,
;--- which becomes EDX
ucs_append:
        push    eax
        push    edx
        mov     dword ptr [ebp+EL_NEXT],eax
        mov     edx,eax
        mov     eax,ebp
        call    hc_link                 ; EAX (old last) -> EDX (new first)
        pop     edx
        pop     eax
        mov     ebp,edx
        ret
ucs_nomem:
        mov     eax,dword ptr [usb_cxfirst]
        call    el_freechain
        mov     dword ptr [usb_cxfirst],0
        popad
        mov     ah,USBE_NOMEM
        stc
        ret

;---------------------------------------------------------------------
; usb_ctl_poll - AL = XS_*; on XS_ERROR AH = USBE_*; on XS_DONE ECX =
;                data bytes moved.  Once it is not pending the chain
;                has been freed and the queue is idle.
;---------------------------------------------------------------------
usb_ctl_poll:
        push    ebx
        push    esi
        push    edi
        cmp     byte ptr [usb_cxbusy],0
        je      ucp_idle
        mov     ebx,[usb_cxdev]
        call    dev_hc
        mov     edi,dword ptr [esi+HC_CTLQ]
        mov     eax,dword ptr [usb_cxfirst]
        mov     edx,dword ptr [usb_cxtime]
        mov     ecx,CTL_TICKS
        push    dword ptr [usb_cxlast]
        pop     dword ptr [usb_cpstatus]
        call    usb_chainpoll           ; -> AL, AH, ECX
        cmp     al,XS_PENDING
        je      ucp_out
        ; over, one way or the other: the queue is stopped and the
        ; chain given back
        push    eax
        push    ecx
        call    hc_stop
        mov     eax,dword ptr [usb_cxfirst]
        call    el_freechain
        mov     dword ptr [usb_cxfirst],0
        mov     byte ptr [usb_cxbusy],0
        pop     ecx
        pop     eax
ucp_out:
        pop     edi
        pop     esi
        pop     ebx
        ret
ucp_idle:
        mov     al,XS_DONE
        xor     ecx,ecx
        jmp     ucp_out

;---------------------------------------------------------------------
; usb_chainpoll - EAX = the first element of a chain on queue EDI of
;                 controller ESI, EDX = the tick it was started at, ECX
;                 = ticks allowed, usb_cpstatus = the status stage's
;                 descriptor (a control transfer) or 0 (a bulk one)
;                 -> AL = XS_*, AH = USBE_*, ECX = data bytes moved
;                 (the setup packet's eight are not data).
;
; A chain is finished when every descriptor is, or when one has
; failed.  An IN descriptor that came back SHORT ends its stage: the
; device has said all it has to say.  In a bulk chain that is the end
; of the transfer, and the caller is told how much came.  In a control
; chain the status stage still has to run, and both controllers stop
; at the short descriptor rather than go on to the next one - UHCI
; because its SPD bit says so, OHCI because a data stage is one
; descriptor here - so the descriptors between it and the status stage
; are marked never-to-run and the queue is pointed at the status
; stage (hc_skipto).  The next poll finds the skipped ones counted as
; done with nothing moved, and the status stage over.
;---------------------------------------------------------------------
usb_chainpoll:
        push    ebx
        push    ebp
        push    edx
        mov     dword ptr [usb_cpticks],ecx
        mov     dword ptr [usb_cpstart],edx
        mov     ebp,eax                 ; EBP walks the chain
        xor     ebx,ebx                 ; EBX = data bytes so far
ucq_lp:
        or      ebp,ebp
        jz      ucq_done
        test    byte ptr [ebp+EL_FLAGS],ELF_SKIP
        jnz     ucq_next
        mov     eax,ebp
        call    hc_tdstat               ; -> AL, AH, ECX
        cmp     al,XS_PENDING
        je      ucq_pending
        cmp     al,XS_ERROR
        je      ucq_error
        test    byte ptr [ebp+EL_FLAGS],ELF_SETUP
        jnz     ucq_next
        movzx   edx,word ptr [ebp+EL_LEN16]
        cmp     ecx,edx
        jbe     ucq_lenok
        mov     ecx,edx                 ; never more than it was given
ucq_lenok:
        add     ebx,ecx
        test    byte ptr [ebp+EL_FLAGS],ELF_IN
        jz      ucq_next
        cmp     cx,word ptr [ebp+EL_LEN16]
        jb      ucq_short
ucq_next:
        mov     ebp,dword ptr [ebp+EL_NEXT]
        jmp     ucq_lp

ucq_short:
        ; a bulk stage ends here and so does the transfer
        cmp     dword ptr [usb_cpstatus],0
        je      ucq_done
        cmp     ebp,dword ptr [usb_cpstatus]
        je      ucq_next                ; the status packet itself: empty
                                        ; is its size, and it is last
        ; a control data stage ended early: skip to the status stage,
        ; unless that was already done by an earlier poll
        mov     eax,dword ptr [ebp+EL_NEXT]
        cmp     eax,dword ptr [usb_cpstatus]
        je      ucq_next                ; nothing between: the controller
                                        ; is on the status stage already
        test    byte ptr [eax+EL_FLAGS],ELF_SKIP
        jnz     ucq_next                ; skipped on an earlier poll
ucq_skip:
        or      byte ptr [eax+EL_FLAGS],ELF_SKIP
        mov     eax,dword ptr [eax+EL_NEXT]
        cmp     eax,dword ptr [usb_cpstatus]
        jne     ucq_skip
        call    hc_skipto               ; EDI = queue, EAX = the status TD
        jmp     ucq_next

ucq_done:
        mov     ecx,ebx
        mov     al,XS_DONE
        xor     ah,ah
        jmp     ucq_out
ucq_error:
IFDEF HANG_E9
        push    eax
        mov     al,'T'
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW0]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW1]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW2]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW3]
        call    e9_hex32
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
        pop     eax
ENDIF
        mov     ecx,ebx
        jmp     ucq_out
ucq_pending:
        ; still going - unless it has been going too long
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,dword ptr [usb_cpstart]
        jae     ucq_tpos
        add     eax,TICKS_PER_DAY
ucq_tpos:
        cmp     eax,dword ptr [usb_cpticks]
        jb      ucq_still
        mov     al,XS_ERROR
        mov     ah,USBE_TIMEOUT
        mov     ecx,ebx
        jmp     ucq_out
ucq_still:
        mov     al,XS_PENDING
        xor     ah,ah
        mov     ecx,ebx
ucq_out:
        pop     edx
        pop     ebp
        pop     ebx
        ret

;---------------------------------------------------------------------
; usb_blk_start - a bulk transfer.  EBX -> device, DL = 0 out, 1 in,
;                 EDI = buffer, ECX = bytes (XFER_MAX at most).  CF with
;                 AH if it could not be built.  Foreground only.
;---------------------------------------------------------------------
usb_blk_start:
        pushad
        pushfd
        cli                             ; the builder's statics are shared
        mov     [usb_bxdev],ebx         ; with the tick's control transfers
        mov     dword ptr [usb_bxlen],ecx
        mov     byte ptr [usb_bxdir],dl
        mov     dword ptr [usb_bxfirst],0
        cmp     dl,0
        jne     ubs_in
        mov     ax,word ptr [ebx+USBD_MAXPOUT]
        mov     word ptr [usb_mkps],ax
        mov     dh,[ebx+USBD_EPOUT]
        mov     al,[ebx+USBD_TOGOUT]
        mov     ah,ELF_BULK
        mov     dl,PID_OUT
        jmp     ubs_build
ubs_in:
        mov     ax,word ptr [ebx+USBD_MAXPIN]
        mov     word ptr [usb_mkps],ax
        mov     dh,[ebx+USBD_EPIN]
        mov     al,[ebx+USBD_TOGIN]
        mov     ah,ELF_BULK OR ELF_IN
        mov     dl,PID_IN
ubs_build:
        mov     byte ptr [usb_bxtog0],al
        call    usb_mkstage             ; -> EAX first, EDX last, CL toggle
        jc      ubs_nomem
        mov     dword ptr [usb_bxfirst],eax
        mov     byte ptr [usb_bxtogend],cl
        call    dev_hc                  ; ESI -> the controller, for hc_term
        mov     eax,edx
        call    hc_term

        mov     ebx,[usb_bxdev]
        call    dev_hc
        mov     edi,dword ptr [esi+HC_BLKQ]
        mov     eax,dword ptr [usb_bxfirst]
        mov     cl,[ebx+USBD_EPOUT]
        mov     dx,word ptr [ebx+USBD_MAXPOUT]
        cmp     byte ptr [usb_bxdir],0
        je      ubs_run
        mov     cl,[ebx+USBD_EPIN]
        or      cl,80h                  ; IN, for the ED's direction field
        mov     dx,word ptr [ebx+USBD_MAXPIN]
ubs_run:
        mov     ch,byte ptr [usb_bxtog0]
        call    hc_run
        mov     eax,dword ptr ds:[BDA_TICKS]
        mov     dword ptr [usb_bxtime],eax
        mov     byte ptr [usb_bxbusy],1
        popfd
        popad
        clc
        ret
ubs_nomem:
        popfd
        popad
        mov     ah,USBE_NOMEM
        stc
        ret

;---------------------------------------------------------------------
; usb_blk_poll - AL = XS_*, AH, ECX = bytes moved.  The device's toggle
;                for the endpoint is brought up to date when the
;                transfer ends: on success from the controller's own
;                count, on a failure left as it was for the recovery
;                to reset.
;---------------------------------------------------------------------
usb_blk_poll:
        push    ebx
        push    esi
        push    edi
        push    edx
        pushfd
        cli                             ; the chain poll's statics are the
        cmp     byte ptr [usb_bxbusy],0 ; tick's too
        je      ubp_idle
        mov     ebx,[usb_bxdev]
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     ubp_gone
        call    dev_hc
        mov     edi,dword ptr [esi+HC_BLKQ]
        mov     eax,dword ptr [usb_bxfirst]
        mov     edx,dword ptr [usb_bxtime]
        mov     ecx,BULK_TICKS
        mov     dword ptr [usb_cpstatus],0
        call    usb_chainpoll
        cmp     al,XS_PENDING
        je      ubp_out
        push    eax
        push    ecx
        cmp     al,XS_DONE
        jne     ubp_notog
        ; the toggle after: what the controller says it is
        call    usb_blktog              ; -> AL
        cmp     byte ptr [usb_bxdir],0
        je      ubp_tout
        mov     [ebx+USBD_TOGIN],al
        jmp     ubp_notog
ubp_tout:
        mov     [ebx+USBD_TOGOUT],al
ubp_notog:
        call    hc_stop
        mov     eax,dword ptr [usb_bxfirst]
        call    el_freechain
        mov     dword ptr [usb_bxfirst],0
        mov     byte ptr [usb_bxbusy],0
        pop     ecx
        pop     eax
ubp_out:
        popfd
        pop     edx
        pop     edi
        pop     esi
        pop     ebx
        ret
ubp_idle:
        mov     al,XS_DONE
        xor     ecx,ecx
        jmp     ubp_out
ubp_gone:
        ; unplugged under the transfer.  The queue is stopped and the
        ; chain freed exactly as for an error; the controller has
        ; nothing to say to a device that is not there
        call    dev_hc
        mov     edi,dword ptr [esi+HC_BLKQ]
        call    hc_stop
        mov     eax,dword ptr [usb_bxfirst]
        call    el_freechain
        mov     dword ptr [usb_bxfirst],0
        mov     byte ptr [usb_bxbusy],0
        mov     al,XS_ERROR
        mov     ah,USBE_GONE
        xor     ecx,ecx
        jmp     ubp_out

;---------------------------------------------------------------------
; usb_blk_abort - stop and free the bulk transfer in progress, so the next
;                 one starts clean.  For the bridged poll that overran its
;                 bound (usbm_xfer); mirrors usb_blk_poll's unplugged-device
;                 cleanup.  Every register kept.
;---------------------------------------------------------------------
usb_blk_abort:
        pushad
        pushfd
        cli
        cmp     byte ptr [usb_bxbusy],0
        je      uba_done
        mov     ebx,[usb_bxdev]
        call    dev_hc                  ; ESI -> the controller
        mov     edi,dword ptr [esi+HC_BLKQ]
        call    hc_stop
        mov     eax,dword ptr [usb_bxfirst]
        call    el_freechain
        mov     dword ptr [usb_bxfirst],0
        mov     byte ptr [usb_bxbusy],0
uba_done:
        popfd
        popad
        ret

;---------------------------------------------------------------------
; usb_blktog - the toggle the bulk endpoint is on after the transfer
;              that just completed.  ESI -> controller, EDI = queue.
;              UHCI: the toggle the stage builder said came next.
;              OHCI: the ED's carry bit.
;---------------------------------------------------------------------
usb_blktog:
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        jne     ubt_oh
        mov     al,byte ptr [usb_bxtogend]
        ret
ubt_oh:
        call    hc_qtoggle
        ret

;---------------------------------------------------------------------
; usb_ctl_acquire - the foreground takes the control queue.  CF if the
;                   tick held it for too long (a second).
; usb_ctl_release - and gives it back.
;---------------------------------------------------------------------
usb_ctl_acquire:
        push    eax
        push    ebx
        mov     ebx,dword ptr ds:[BDA_TICKS]
uca_try:
        cli
        cmp     byte ptr [usb_ctlown],0
        jne     uca_busy
        mov     byte ptr [usb_ctlown],2
        sti
        pop     ebx
        pop     eax
        clc
        ret
uca_busy:
        sti
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,ebx
        jae     uca_pos
        add     eax,TICKS_PER_DAY
uca_pos:
        cmp     eax,CTL_TICKS + 2
        jb      uca_try
        pop     ebx
        pop     eax
        stc
        ret

usb_ctl_release:
        mov     byte ptr [usb_ctlown],0
        ret

;---------------------------------------------------------------------
; usb_ctl_sync - a control transfer, start to finish, for the
;                foreground: EBX -> device, ESI -> setup, EDI -> data,
;                ECX = length.  -> AL = XS_DONE or XS_ERROR with AH,
;                ECX = bytes.  Takes and gives back the control queue.
;---------------------------------------------------------------------
usb_ctl_sync:
        push    ebx
        call    usb_ctl_acquire
        jc      ucy_busy
        call    usb_ctl_start
        jc      ucy_fail
ucy_wait:
        sti
        call    usb_ctl_poll
        cmp     al,XS_PENDING
        je      ucy_wait
        call    usb_ctl_release
        pop     ebx
        ret
ucy_fail:
        call    usb_ctl_release
        mov     al,XS_ERROR
        xor     ecx,ecx
        pop     ebx
        ret
ucy_busy:
        mov     al,XS_ERROR
        mov     ah,USBE_TIMEOUT
        xor     ecx,ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; usb_setup - fill the setup packet at usb_setupbuf: AL = bmRequestType,
;             AH = bRequest, CX = wValue, DX = wIndex, DI = wLength.
;             ESI -> it on return.  DI is the length, so the data
;             pointer goes into EDI AFTER this, never before.
;---------------------------------------------------------------------
usb_setup:
        mov     esi,OFFSET usb_setupbuf
        mov     [esi],al
        mov     [esi+1],ah
        mov     [esi+2],cx
        mov     [esi+4],dx
        mov     [esi+6],di
        ret

;=====================================================================
; THE TICK.  Called by irq_timer once an interrupt, interrupts off,
; everything preserved.  What it does, in order: controller
; housekeeping, the reports the HID devices have sent, the queues of
; devices that have gone, and then ONE piece of the slow work - the
; outstanding job's completion, a step of the enumeration in hand, a
; look at the ports, a hub's port, a keyboard's lights.
;=====================================================================
usb_tick:
        cmp     byte ptr [usb_started],0
        je      ut_ret
        pushad
IFDEF HANG_E9
        cmp     byte ptr ds:[GV_BOOTDRV],80h
        je      utw_ok
        cmp     byte ptr [usb_wflag],0
        jne     utw_ok
        mov     byte ptr [usb_wflag],1
        mov     al,'W'
        out     0E9h,al
        movzx   eax,byte ptr [usb_job]
        call    e9_hex8
        movzx   eax,byte ptr [usb_enumdev]
        call    e9_hex8
        mov     al,byte ptr [usb_enumdev]
        cmp     al,0FFh
        je      utw_nosub
        call    dev_row
        movzx   eax,byte ptr [ebx+USBD_SUB]
        call    e9_hex8
utw_nosub:
        mov     al,' '
        out     0E9h,al
        movzx   eax,byte ptr [usb_hubstep]
        call    e9_hex8
        movzx   eax,byte ptr [usb_hubidx]
        call    e9_hex8
        mov     al,' '
        out     0E9h,al
        movzx   eax,byte ptr ds:[GV_BOOTDRV]
        call    e9_hex8
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
utw_ok:
ENDIF
        call    usb_ackall
        call    usbhid_harvest
        call    usbhid_repeat
        call    usb_reap

        ; a job out: has it finished?
        cmp     byte ptr [usb_job],JOB_NONE
        je      ut_nojob
        call    usb_ctl_poll
        cmp     al,XS_PENDING
        je      ut_out
        mov     byte ptr [usb_ctlown],0
        mov     bl,byte ptr [usb_job]
        mov     byte ptr [usb_job],JOB_NONE
        cmp     bl,JOB_ENUM
        je      usb_enum_cont
        cmp     bl,JOB_HUB
        je      usb_hub_cont
        cmp     bl,JOB_LED
        je      usbhid_led_cont
        jmp     ut_out

ut_nojob:
        cmp     byte ptr [usb_ctlown],2
        je      ut_out                  ; the foreground has the control
                                        ; queue: nothing slow this tick
        cmp     byte ptr [usb_enumdev],0FFh
        jne     usb_enum_step
        call    usb_scanports
        jc      ut_out                  ; an enumeration began
        call    usb_hubpoll
        jc      ut_out                  ; a hub job began
        call    usbhid_leds
ut_out:
        popad
ut_ret:
        ret

;---------------------------------------------------------------------
; usb_ackall - every controller's once-a-tick housekeeping.
;---------------------------------------------------------------------
usb_ackall:
        push    ecx
        push    esi
        xor     ecx,ecx
uaa_lp:
        cmp     cl,[usb_hcn]
        jae     uaa_out
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        test    byte ptr [esi+HC_FLAGS],USBHF_OWNED
        jz      uaa_next
        call    hc_ack
uaa_next:
        inc     ecx
        jmp     uaa_lp
uaa_out:
        pop     esi
        pop     ecx
        ret

;---------------------------------------------------------------------
; usb_reap - a device that went (USBS_GONE) had its queues unlinked
;            when it went; a tick later the controller cannot be
;            inside them any more, and they go back to the pool.
;---------------------------------------------------------------------
usb_reap:
        push    ebx
        push    ecx
        push    eax
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
urp_lp:
        cmp     byte ptr [ebx+USBD_STATE],USBS_GONE
        jne     urp_next
        mov     eax,dword ptr [ebx+USBD_RING]
        or      eax,eax
        jz      urp_noring
        call    usb_freering
urp_noring:
        mov     eax,dword ptr [ebx+USBD_QH]
        or      eax,eax
        jz      urp_noqh
        call    el_free
        mov     dword ptr [ebx+USBD_QH],0
urp_noqh:
        mov     byte ptr [ebx+USBD_STATE],USBS_FREE
urp_next:
        add     ebx,USBD_LEN
        loop    urp_lp
        pop     eax
        pop     ecx
        pop     ebx
        ret

;--- usb_freering - EAX = the first element of a ring of RING_N, each
;--- linked to the next by EL_NEXT and the last back to the first
usb_freering:
        push    ebx
        push    ecx
        mov     ecx,RING_N
ufr_lp:
        mov     ebx,dword ptr [eax+EL_NEXT]
        call    el_free
        mov     eax,ebx
        loop    ufr_lp
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; THE PORTS.  Every root port of every controller is looked at every
; tick: a change bit set, or a connection the table does not know of,
; starts an enumeration; a disconnection where the table has a device
; takes the device down.  One enumeration at a time, because every
; device starts life at address 0 and two of them there would answer
; together.
;
; usb_scanports - CF if an enumeration was begun.
;=====================================================================
usb_scanports:
        push    ecx
        push    esi
        push    eax
        push    ebx
        push    edx
        xor     ecx,ecx
usp_hc:
        cmp     cl,[usb_hcn]
        jae     usp_none
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        test    byte ptr [esi+HC_FLAGS],USBHF_OWNED
        jz      usp_hcnext
        push    ecx
        mov     cl,1                    ; CL = the port
usp_port:
        cmp     cl,[esi+HC_NPORTS]
        ja      usp_hcdone
        call    hc_portst               ; -> AL
        mov     dl,al
        ; which device sits on this port, if any
        mov     ch,cl
        mov     eax,esi
        sub     eax,OFFSET usb_hcs
        push    edx
        push    ebx
        xor     edx,edx
        mov     ebx,HC_LEN
        div     ebx
        pop     ebx
        pop     edx
        mov     ah,al                   ; AH = the controller's index
        mov     al,ch                   ; AL = the port
        call    usb_findport            ; -> EBX -> device, or CF
        jnc     usp_known
        ; nothing of ours there: a connection, with or without the
        ; change bit, is a new device
        test    dl,PST_CONNECT
        jz      usp_ack
        cmp     byte ptr [usb_forcescan],0
        jne     usp_new
        test    dl,PST_CHANGE
        jz      usp_new                 ; found connected with nothing
                                        ; said: it was there before we
                                        ; looked (a park, a boot)
usp_new:
        call    hc_portack
        mov     al,ch
        call    usb_enum_begin          ; AH = controller, AL = port, CF
                                        ; if no slot was free
        jc      usp_portnext
        pop     ecx
        jmp     usp_began
usp_known:
        ; a device is registered on the port.  Gone, or come back (a
        ; change bit with the connection still there is a device
        ; that was pulled and pushed in again between two ticks)?
        test    dl,PST_CHANGE
        jnz     usp_dropit
        test    dl,PST_CONNECT
        jnz     usp_portnext
usp_dropit:
        call    hc_portack
        call    usb_detach
        test    dl,PST_CONNECT
        jz      usp_portnext
        mov     al,ch
        call    usb_enum_begin
        jc      usp_portnext
        pop     ecx
        jmp     usp_began
usp_ack:
        test    dl,PST_CHANGE
        jz      usp_portnext
        call    hc_portack
usp_portnext:
        inc     cl
        jmp     usp_port
usp_hcdone:
        pop     ecx
usp_hcnext:
        inc     ecx
        jmp     usp_hc
usp_none:
        mov     byte ptr [usb_forcescan],0
        pop     edx
        pop     ebx
        pop     eax
        pop     esi
        pop     ecx
        clc
        ret
usp_began:
        pop     edx
        pop     ebx
        pop     eax
        pop     esi
        pop     ecx
        stc
        ret

;---------------------------------------------------------------------
; usb_findport - AH = controller, AL = root port -> EBX -> the device
;                registered there (any state but free), or CF.
; usb_findhubport - AH = a hub's slot, AL = its port -> the same.
;---------------------------------------------------------------------
usb_findport:
        push    ecx
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
ufp_lp:
        cmp     byte ptr [ebx+USBD_STATE],USBS_FREE
        je      ufp_next
        cmp     byte ptr [ebx+USBD_PARENT],0FFh
        jne     ufp_next
        cmp     [ebx+USBD_HC],ah
        jne     ufp_next
        cmp     [ebx+USBD_PORT],al
        je      ufp_hit
ufp_next:
        add     ebx,USBD_LEN
        loop    ufp_lp
        pop     ecx
        stc
        ret
ufp_hit:
        pop     ecx
        clc
        ret

usb_findhubport:
        push    ecx
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
ufhp_lp:
        cmp     byte ptr [ebx+USBD_STATE],USBS_FREE
        je      ufhp_next
        cmp     [ebx+USBD_PARENT],ah
        jne     ufhp_next
        cmp     [ebx+USBD_PORT],al
        je      ufhp_hit
ufhp_next:
        add     ebx,USBD_LEN
        loop    ufhp_lp
        pop     ecx
        stc
        ret
ufhp_hit:
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; usb_detach - EBX -> a device: gone.  Its interrupt queue comes out of
;              the schedule now and is freed next tick (usb_reap); a
;              keyboard's keys come up; a disk's unit is emptied; a
;              hub's children go with it.  Everything preserved.
;---------------------------------------------------------------------
usb_detach:
        pushad
        cmp     byte ptr [ebx+USBD_STATE],USBS_FREE
        je      udx_out
        cmp     byte ptr [ebx+USBD_STATE],USBS_GONE
        je      udx_out
        ; the one being enumerated?  Then there is nothing to
        ; enumerate any more
        call    dev_slot
        cmp     al,byte ptr [usb_enumdev]
        jne     udx_notenum
        mov     byte ptr [usb_enumdev],0FFh
        cmp     byte ptr [usb_job],JOB_ENUM
        jne     udx_notenum
        ; its control transfer is out: stopped and freed here, since
        ; the continuation would otherwise act on a device that has
        ; gone.  The queue is the controller's and the chain is ours.
        push    ebx
        call    usb_ctl_abort
        pop     ebx
        mov     byte ptr [usb_job],JOB_NONE
        mov     byte ptr [usb_ctlown],0
udx_notenum:
        cmp     byte ptr [usb_job],JOB_HUB
        jne     udx_nothubjob
        cmp     al,byte ptr [usb_jobdev]
        jne     udx_nothubjob
        push    ebx
        call    usb_ctl_abort
        pop     ebx
        mov     byte ptr [usb_job],JOB_NONE
        mov     byte ptr [usb_ctlown],0
udx_nothubjob:
        cmp     byte ptr [usb_job],JOB_LED
        jne     udx_notledjob
        cmp     al,byte ptr [usb_jobdev]
        jne     udx_notledjob
        push    ebx
        call    usb_ctl_abort
        pop     ebx
        mov     byte ptr [usb_job],JOB_NONE
        mov     byte ptr [usb_ctlown],0
udx_notledjob:

        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     udx_queues
        mov     al,[ebx+USBD_CLASS]
        cmp     al,USBC_KBD
        je      udx_kbd
        cmp     al,USBC_MOUSE
        je      udx_queues
        cmp     al,USBC_DISK
        je      udx_disk
        cmp     al,USBC_HUB
        je      udx_hub
        jmp     udx_queues
udx_kbd:
        call    usbhid_kbdgone          ; every key it held comes up
        jmp     udx_queues
udx_disk:
        call    usbm_detach
        jmp     udx_queues
udx_hub:
        ; the children first, each the same way
        call    dev_slot
        mov     ah,al
        push    ebx
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
udx_child:
        cmp     byte ptr [ebx+USBD_STATE],USBS_FREE
        je      udx_cnext
        cmp     [ebx+USBD_PARENT],ah
        jne     udx_cnext
        call    usb_detach
udx_cnext:
        add     ebx,USBD_LEN
        loop    udx_child
        pop     ebx

udx_queues:
        mov     edi,dword ptr [ebx+USBD_QH]
        or      edi,edi
        jz      udx_noq
        call    dev_hc
        call    hc_intrdel
udx_noq:
        mov     byte ptr [ebx+USBD_STATE],USBS_GONE
udx_out:
        popad
        ret

;--- usb_ctl_abort - the control transfer the tick has out, stopped
;--- and freed without a continuation
usb_ctl_abort:
        push    eax
        push    ebx
        push    esi
        push    edi
        cmp     byte ptr [usb_cxbusy],0
        je      uab_out
        mov     ebx,[usb_cxdev]
        call    dev_hc
        mov     edi,dword ptr [esi+HC_CTLQ]
        call    hc_stop
        mov     eax,dword ptr [usb_cxfirst]
        call    el_freechain
        mov     dword ptr [usb_cxfirst],0
        mov     byte ptr [usb_cxbusy],0
uab_out:
        pop     edi
        pop     esi
        pop     ebx
        pop     eax
        ret

;=====================================================================
; ENUMERATION.  One device at a time (usb_enumdev), a step a tick or
; a step a completed transfer.  usb_enum_step is reached from the tick
; when no job is out; usb_enum_cont when the job it started has ended,
; with AL = XS_* and AH, ECX from the poll.  Both end by jumping to
; ut_out, which is why they are not CALLed: they are the tail of the
; tick.
;=====================================================================

;---------------------------------------------------------------------
; usb_enum_begin - a device has appeared: AH = controller, AL = root
;                  port; or, with AH = 80h + hub slot, on a hub's port.
;                  A free slot is taken and the debounce begins.  CF if
;                  no slot is free.  Preserves everything but flags.
;---------------------------------------------------------------------
usb_enum_begin:
        pushad
        mov     dl,ah
        mov     dh,al
        mov     ebx,OFFSET usb_devs
        xor     ecx,ecx
ueb_lp:
        cmp     byte ptr [ebx+USBD_STATE],USBS_FREE
        je      ueb_got
        add     ebx,USBD_LEN
        inc     ecx
        cmp     ecx,USB_MAXDEV
        jb      ueb_lp
        popad
        stc
        ret
ueb_got:
        ; a clean row
        push    ecx
        mov     edi,ebx
        mov     ecx,USBD_LEN / 4
        xor     eax,eax
        rep     stosd
        pop     ecx
        mov     byte ptr [ebx+USBD_PARENT],0FFh
        test    dl,80h
        jz      ueb_root
        and     dl,7Fh
        mov     [ebx+USBD_PARENT],dl    ; a hub's child: its controller
        push    ebx                     ; is the hub's
        mov     al,dl
        call    dev_row
        mov     al,[ebx+USBD_HC]
        pop     ebx
        mov     dl,al
ueb_root:
        mov     [ebx+USBD_HC],dl
        mov     [ebx+USBD_PORT],dh
        mov     byte ptr [ebx+USBD_STATE],USBS_ENUM
        mov     byte ptr [ebx+USBD_SUB],ES_DEBOUNCE
        mov     eax,dword ptr ds:[BDA_TICKS]
        mov     dword ptr [ebx+USBD_TIME],eax
        mov     byte ptr [usb_enumdev],cl
        popad
        clc
        ret

;--- ue_elapsed - CF clear if ECX ticks have passed since USBD_TIME
ue_elapsed:
        push    eax
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,dword ptr [ebx+USBD_TIME]
        jae     uee_pos
        add     eax,TICKS_PER_DAY
uee_pos:
        cmp     eax,ecx
        jb      uee_not
        pop     eax
        clc
        ret
uee_not:
        pop     eax
        stc
        ret

;--- ue_stamp - USBD_TIME = now
ue_stamp:
        push    eax
        mov     eax,dword ptr ds:[BDA_TICKS]
        mov     dword ptr [ebx+USBD_TIME],eax
        pop     eax
        ret

;--- ue_ctl - start a control transfer for the device being enumerated
;--- as JOB_ENUM: ESI -> setup, EDI -> data, ECX = length.  On a
;--- failure to build, the step is failed as an error would fail it.
ue_ctl:
        mov     byte ptr [usb_ctlown],1
        call    usb_ctl_start
        jc      uec_fail
        mov     byte ptr [usb_job],JOB_ENUM
        call    dev_slot
        mov     byte ptr [usb_jobdev],al
        jmp     ut_out
uec_fail:
        mov     byte ptr [usb_ctlown],0
        jmp     usb_enum_fail

;---------------------------------------------------------------------
; usb_enum_step - the device at usb_enumdev, no job out.
;---------------------------------------------------------------------
usb_enum_step:
        mov     al,byte ptr [usb_enumdev]
        call    dev_row
        call    dev_hc
        mov     cl,[ebx+USBD_PORT]
        mov     al,[ebx+USBD_SUB]
        cmp     al,ES_DEBOUNCE
        je      ues_debounce
        cmp     al,ES_RESET
        je      ues_reset
        cmp     al,ES_RESETEND
        je      ues_resetend
        cmp     al,ES_RECOVER
        je      ues_recover
        cmp     al,ES_ADDRWAIT
        je      ues_addrwait
        cmp     al,ES_HUBPWR
        je      ues_hubpwr
        cmp     al,ES_HUBWAIT
        je      ues_hubwait
        cmp     al,ES_HRESETPOLL
        je      ues_hresetpoll
        ; any other substate has a job out, and this is not reached
        jmp     ut_out

ues_debounce:
        ; two ticks of a steady connection, then the reset
        mov     ecx,2
        call    ue_elapsed
        jc      ut_out
        cmp     byte ptr [ebx+USBD_PARENT],0FFh
        jne     ues_hubreset
        mov     cl,[ebx+USBD_PORT]
        call    hc_portst
        test    al,PST_CONNECT
        jz      usb_enum_vanished
        call    hc_portreset
        call    ue_stamp
        mov     byte ptr [ebx+USBD_SUB],ES_RESET
        jmp     ut_out

ues_reset:
        ; UHCI: at least 50ms asserted - a tick is 55 - then released
        ; and the port enabled.  OHCI: the controller times it and says
        ; when it is over.
        mov     ecx,1
        call    ue_elapsed
        jc      ut_out
        mov     cl,[ebx+USBD_PORT]
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        je      ues_uhend
        call    hc_portst
        test    al,PST_RESETDONE
        jnz     ues_ohdone
        mov     ecx,4                   ; not finished in 200ms: give up
        call    ue_elapsed
        jc      ut_out
        jmp     usb_enum_fail
ues_ohdone:
        call    hc_portack
        jmp     ues_enabled
ues_uhend:
        call    hc_portenable
ues_enabled:
        call    ue_stamp
        mov     byte ptr [ebx+USBD_SUB],ES_RECOVER
        jmp     ut_out

ues_resetend:                           ; (UHCI's enable is immediate)
        jmp     ues_enabled

ues_recover:
        ; a tick after the reset: the device may be spoken to.  Its
        ; speed is the port's to say.
        mov     ecx,1
        call    ue_elapsed
        jc      ut_out
        cmp     byte ptr [ebx+USBD_PARENT],0FFh
        jne     ues_recover2            ; a hub child's speed was noted
        mov     cl,[ebx+USBD_PORT]
        call    hc_portst
        test    al,PST_CONNECT
        jz      usb_enum_vanished
        test    al,PST_LOWSPEED
        jz      ues_recover2
        or      byte ptr [ebx+USBD_FLAGS],USBDF_LOWSPEED
        mov     byte ptr [ebx+USBD_SPEED],1
ues_recover2:
        mov     byte ptr [ebx+USBD_ADDR],0
        mov     byte ptr [ebx+USBD_MAXP0],8
        mov     byte ptr [ebx+USBD_SUB],ES_GETDESC8
        ; GET_DESCRIPTOR(device), the first eight bytes: the eighth is
        ; endpoint 0's packet size, which the rest needs
        mov     al,80h
        mov     ah,UREQ_GETDESC
        mov     cx,UDT_DEVICE SHL 8
        xor     dx,dx
        mov     di,8
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     ecx,8
        jmp     ue_ctl

ues_addrwait:
        mov     ecx,1
        call    ue_elapsed
        jc      ut_out
        mov     byte ptr [ebx+USBD_SUB],ES_GETDEV
        mov     al,80h
        mov     ah,UREQ_GETDESC
        mov     cx,UDT_DEVICE SHL 8
        xor     dx,dx
        mov     di,18
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     ecx,18
        jmp     ue_ctl

ues_hubpwr:
        ; SET_FEATURE(PORT_POWER) on the next port
        mov     al,[ebx+USBD_HPORT]
        inc     al
        cmp     al,[ebx+USBD_UNIT]      ; the port count, for a hub
        ja      ues_hubpowered
        mov     [ebx+USBD_HPORT],al
        movzx   edx,al
        mov     al,23h                  ; class, other (a port)
        mov     ah,UREQ_SETFEAT
        mov     cx,HUBF_PORTPOWER
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        jmp     ue_ctl
ues_hubpowered:
        call    ue_stamp
        mov     byte ptr [ebx+USBD_SUB],ES_HUBWAIT
        jmp     ut_out

ues_hubwait:
        ; power-good, in 2ms units, rounded up to ticks, and one more
        movzx   ecx,byte ptr [ebx+USBD_HGOOD]
        shl     ecx,1
        add     ecx,54
        push    eax
        mov     eax,ecx
        xor     edx,edx
        mov     ecx,55
        div     ecx
        mov     ecx,eax
        pop     eax
        inc     ecx
        call    ue_elapsed
        jc      ut_out
        mov     byte ptr [ebx+USBD_HPORT],0
        jmp     usb_enum_ready

ues_hubreset:
        ; a hub's child: the hub is told to reset the port
        mov     byte ptr [ebx+USBD_SUB],ES_HRESET
        movzx   edx,byte ptr [ebx+USBD_PORT]
        mov     al,23h
        mov     ah,UREQ_SETFEAT
        mov     cx,HUBF_PORTRESET
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        push    ebx
        mov     al,[ebx+USBD_PARENT]    ; the transfer goes to the HUB
        call    dev_row
        mov     byte ptr [usb_ctlown],1
        call    usb_ctl_start
        pop     ebx
        jc      ues_hrfail
        mov     byte ptr [usb_job],JOB_ENUM
        call    dev_slot
        mov     byte ptr [usb_jobdev],al
        call    ue_stamp
        jmp     ut_out
ues_hrfail:
        mov     byte ptr [usb_ctlown],0
        jmp     usb_enum_fail

ues_hresetpoll:
        ; a tick on: ask the hub whether the reset has finished
        mov     ecx,1
        call    ue_elapsed
        jc      ut_out
        mov     ecx,8
        call    ue_elapsed
        jnc     usb_enum_fail           ; not in 400ms: no
        movzx   edx,byte ptr [ebx+USBD_PORT]
        mov     al,0A3h                 ; class, other, IN
        mov     ah,UREQ_GETSTATUS
        xor     cx,cx
        mov     di,4
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     ecx,4
        push    ebx
        mov     al,[ebx+USBD_PARENT]
        call    dev_row
        mov     byte ptr [usb_ctlown],1
        call    usb_ctl_start
        pop     ebx
        jc      ues_hrfail
        mov     byte ptr [usb_job],JOB_ENUM
        call    dev_slot
        mov     byte ptr [usb_jobdev],al
        jmp     ut_out

;---------------------------------------------------------------------
; usb_enum_cont - the step's transfer has ended: AL = XS_*, AH, ECX.
;---------------------------------------------------------------------
usb_enum_cont:
        mov     dl,al
        mov     dh,ah
        mov     al,byte ptr [usb_enumdev]
        cmp     al,0FFh
        je      ut_out                  ; it went while the job was out
        call    dev_row
        call    dev_hc
IFDEF HANG_E9
        call    usb_trace_cont
ENDIF
        cmp     dl,XS_DONE
        je      uec_ok
        ; GET_MAX_LUN is the one request a device may refuse: a stall
        ; there means one logical unit, and is an answer
        cmp     byte ptr [ebx+USBD_SUB],ES_CLASS1
        jne     usb_enum_fail
        cmp     byte ptr [ebx+USBD_CLASS],USBC_DISK
        jne     usb_enum_fail
        cmp     dh,USBE_STALL
        jne     usb_enum_fail
        xor     ecx,ecx
uec_ok:
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC            ; EDI -> what came back
        mov     al,[ebx+USBD_SUB]
        cmp     al,ES_GETDESC8
        je      uec_getdesc8
        cmp     al,ES_SETADDR
        je      uec_setaddr
        cmp     al,ES_GETDEV
        je      uec_getdev
        cmp     al,ES_GETSTR
        je      uec_getstr
        cmp     al,ES_GETCFG9
        je      uec_getcfg9
        cmp     al,ES_GETCFG
        je      uec_getcfg
        cmp     al,ES_SETCFG
        je      uec_setcfg
        cmp     al,ES_CLASS1
        je      uec_class1
        cmp     al,ES_CLASS2
        je      uec_class2
        cmp     al,ES_HUBPWR
        je      ues_hubpwr              ; the next port's
        cmp     al,ES_HRESET
        je      uec_hreset
        cmp     al,ES_HRESETPOLL
        je      uec_hresetpoll
        cmp     al,ES_HRESETACK
        je      uec_hresetack
        jmp     usb_enum_fail

uec_getdesc8:
        cmp     ecx,8
        jb      usb_enum_fail
        mov     al,[edi+7]              ; bMaxPacketSize0
        cmp     al,8
        jb      usb_enum_fail
        mov     [ebx+USBD_MAXP0],al
        ; SET_ADDRESS: the slot's number and one
        call    dev_slot
        inc     al
        movzx   ecx,al
        mov     al,00h
        mov     ah,UREQ_SETADDR
        xor     dx,dx
        xor     di,di
        call    usb_setup
        mov     byte ptr [ebx+USBD_SUB],ES_SETADDR
        xor     ecx,ecx
        jmp     ue_ctl

uec_setaddr:
        call    dev_slot
        inc     al
        mov     [ebx+USBD_ADDR],al
        mov     byte ptr [ebx+USBD_TRIES],0
        call    ue_stamp
        mov     byte ptr [ebx+USBD_SUB],ES_ADDRWAIT
        jmp     ut_out

uec_getdev:
        cmp     ecx,18
        jb      usb_enum_fail
        mov     ax,[edi+8]
        mov     [ebx+USBD_VID],ax
        mov     ax,[edi+10]
        mov     [ebx+USBD_PID],ax
        mov     al,[edi+15]             ; iProduct
        mov     [ebx+USBD_IPROD],al
        mov     byte ptr [ebx+USBD_CLASS],USBC_OTHER
        cmp     byte ptr [edi+4],9      ; bDeviceClass: a hub says so here
        jne     uec_nothub
        mov     byte ptr [ebx+USBD_CLASS],USBC_HUB
uec_nothub:
        or      al,al
        jz      uec_nostring
        ; the product string, English, 64 bytes of UTF-16 at most
        mov     byte ptr [ebx+USBD_SUB],ES_GETSTR
        movzx   ecx,al
        or      cx,UDT_STRING SHL 8
        mov     al,80h
        mov     ah,UREQ_GETDESC
        mov     dx,0409h
        mov     di,66
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     ecx,66
        jmp     ue_ctl
uec_nostring:
uec_getstr_next:
        mov     byte ptr [ebx+USBD_SUB],ES_GETCFG9
        mov     al,80h
        mov     ah,UREQ_GETDESC
        mov     cx,UDT_CONFIG SHL 8
        xor     dx,dx
        mov     di,9
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     ecx,9
        jmp     ue_ctl

uec_getstr:
        ; UTF-16 to ASCII, what fits, blanks and all
        push    esi
        push    edi
        lea     esi,[edi+2]
        cmp     ecx,2                   ; ECX = what came back
        jb      uec_strdone
        movzx   eax,byte ptr [edi]      ; bLength
        cmp     eax,ecx
        jbe     uec_strblen
        mov     eax,ecx                 ; a short answer: only what came
uec_strblen:
        mov     ecx,eax
        sub     ecx,2
        jbe     uec_strdone
        shr     ecx,1
        jz      uec_strdone             ; an odd byte is no character
        cmp     ecx,31
        jbe     uec_strlen
        mov     ecx,31
uec_strlen:
        lea     edi,[ebx+USBD_NAME]
uec_strcp:
        mov     ax,[esi]
        add     esi,2
        cmp     ax,20h
        jb      uec_strq
        cmp     ax,7Eh
        jbe     uec_strok
uec_strq:
        mov     al,'?'
uec_strok:
        mov     [edi],al
        inc     edi
        loop    uec_strcp
        mov     byte ptr [edi],0
uec_strdone:
        pop     edi
        pop     esi
        jmp     uec_getstr_next

uec_getcfg9:
        cmp     ecx,9
        jb      usb_enum_fail
        mov     al,[edi+5]              ; bConfigurationValue
        mov     [ebx+USBD_CFGVAL],al
        movzx   ecx,word ptr [edi+2]    ; wTotalLength
        cmp     ecx,9
        jb      usb_enum_fail
        cmp     ecx,512
        jbe     uec_cfgfits
        mov     ecx,512
uec_cfgfits:
        mov     byte ptr [ebx+USBD_SUB],ES_GETCFG
        push    ecx
        mov     al,80h
        mov     ah,UREQ_GETDESC
        mov     cx,UDT_CONFIG SHL 8
        xor     dx,dx
        pop     edi
        push    edi
        call    usb_setup
        pop     ecx
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        jmp     ue_ctl

uec_getcfg:
        ; walk the descriptors for an interface this kernel knows how
        ; to use, and its endpoints
        call    usb_parsecfg            ; ECX = bytes; sets class, iface,
                                        ; endpoints
        mov     byte ptr [ebx+USBD_SUB],ES_SETCFG
        movzx   ecx,byte ptr [ebx+USBD_CFGVAL]
        mov     al,00h
        mov     ah,UREQ_SETCFG
        xor     dx,dx
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        jmp     ue_ctl

uec_setcfg:
        mov     byte ptr [ebx+USBD_TOGIN],0
        mov     byte ptr [ebx+USBD_TOGOUT],0
        mov     al,[ebx+USBD_CLASS]
        cmp     al,USBC_KBD
        je      uec_hid1
        cmp     al,USBC_MOUSE
        je      uec_hid1
        cmp     al,USBC_DISK
        je      uec_disk1
        cmp     al,USBC_HUB
        je      uec_hub1
        jmp     usb_enum_ready          ; something else: in the table,
                                        ; and left alone
uec_hid1:
        ; SET_PROTOCOL(boot)
        mov     byte ptr [ebx+USBD_SUB],ES_CLASS1
        movzx   edx,byte ptr [ebx+USBD_IFACE]
        mov     al,21h
        mov     ah,UREQ_SETPROTO
        xor     cx,cx
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        jmp     ue_ctl
uec_disk1:
        ; GET_MAX_LUN - a stall is an answer (one LUN)
        mov     byte ptr [ebx+USBD_SUB],ES_CLASS1
        movzx   edx,byte ptr [ebx+USBD_IFACE]
        mov     al,0A1h
        mov     ah,UREQ_GETMAXLUN
        xor     cx,cx
        mov     di,1
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     byte ptr [edi],0
        mov     ecx,1
        jmp     ue_ctl
uec_hub1:
        ; the hub descriptor: how many ports, how long for power
        mov     byte ptr [ebx+USBD_SUB],ES_CLASS1
        mov     al,0A0h
        mov     ah,UREQ_GETDESC
        mov     cx,UDT_HUB SHL 8
        xor     dx,dx
        mov     di,9
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     ecx,9
        jmp     ue_ctl

uec_class1:
        mov     al,[ebx+USBD_CLASS]
        cmp     al,USBC_DISK
        je      uec_disk2
        cmp     al,USBC_HUB
        je      uec_hub2
        ; HID: SET_IDLE(0) - reports only when something changes
        mov     byte ptr [ebx+USBD_SUB],ES_CLASS2
        movzx   edx,byte ptr [ebx+USBD_IFACE]
        mov     al,21h
        mov     ah,UREQ_SETIDLE
        xor     cx,cx
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        jmp     ue_ctl
uec_disk2:
        ; a disk is a unit from here; the medium is asked about when
        ; the block layer first wants it (usbm_probe)
        call    usbm_attach             ; CF: no unit free
        jc      usb_enum_other
        jmp     usb_enum_ready
uec_hub2:
        cmp     ecx,7
        jb      usb_enum_fail
        mov     al,[edi+2]              ; bNbrPorts
        cmp     al,15
        jbe     uec_hubports
        mov     al,15
uec_hubports:
        mov     [ebx+USBD_UNIT],al
        mov     al,[edi+5]              ; bPwrOn2PwrGood
        mov     [ebx+USBD_HGOOD],al
        mov     byte ptr [ebx+USBD_HPORT],0
        mov     byte ptr [ebx+USBD_SUB],ES_HUBPWR
        jmp     ues_hubpwr

uec_class2:
        ; HID: configured.  The interrupt queue and its ring go into
        ; the schedule and the reports start arriving.
        call    usbhid_arm              ; CF: pool empty
        jc      usb_enum_fail
        jmp     usb_enum_ready

uec_hreset:
        ; PORT_RESET accepted: poll for its end, from the next tick
        call    ue_stamp
        mov     byte ptr [ebx+USBD_SUB],ES_HRESETPOLL
        jmp     ut_out

uec_hresetpoll:
        cmp     ecx,4
        jb      usb_enum_fail
        mov     ax,[edi+2]              ; wPortChange
        test    ax,HPC_RESET
        jz      ut_out                  ; not yet: ues_hresetpoll asks again
        mov     ax,[edi]                ; wPortStatus
        test    ax,HPS_CONNECT
        jz      usb_enum_vanished
        test    ax,HPS_LOWSPEED
        jz      uec_hrfs
        or      byte ptr [ebx+USBD_FLAGS],USBDF_LOWSPEED
        mov     byte ptr [ebx+USBD_SPEED],1
uec_hrfs:
        ; CLEAR_FEATURE(C_PORT_RESET)
        mov     byte ptr [ebx+USBD_SUB],ES_HRESETACK
        movzx   edx,byte ptr [ebx+USBD_PORT]
        mov     al,23h
        mov     ah,UREQ_CLRFEAT
        mov     cx,HUBF_C_RESET
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        push    ebx
        mov     al,[ebx+USBD_PARENT]
        call    dev_row
        mov     byte ptr [usb_ctlown],1
        call    usb_ctl_start
        pop     ebx
        jc      ues_hrfail
        mov     byte ptr [usb_job],JOB_ENUM
        call    dev_slot
        mov     byte ptr [usb_jobdev],al
        jmp     ut_out

uec_hresetack:
        call    ue_stamp
        mov     byte ptr [ebx+USBD_SUB],ES_RECOVER
        jmp     ut_out

;--- usb_enum_ready - configured: in service from here
usb_enum_ready:
        mov     byte ptr [ebx+USBD_STATE],USBS_READY
        mov     byte ptr [ebx+USBD_SUB],0
        mov     byte ptr [usb_enumdev],0FFh
        cmp     byte ptr [ebx+USBD_CLASS],USBC_MOUSE
        jne     ut_out
        cmp     byte ptr ds:[ms_hw],MS_NONE
        jne     ut_out
        mov     byte ptr ds:[ms_hw],MS_USB      ; the machine has a mouse now
        mov     byte ptr ds:[ms_nbtn],3
        jmp     ut_out

;--- usb_enum_other - configured, but nothing this kernel drives
usb_enum_other:
        mov     byte ptr [ebx+USBD_CLASS],USBC_OTHER
        jmp     usb_enum_ready

;--- usb_enum_vanished - the port went empty in the middle of it all
usb_enum_vanished:
        mov     byte ptr [ebx+USBD_STATE],USBS_FREE
        mov     byte ptr [usb_enumdev],0FFh
        jmp     ut_out

;--- usb_enum_fail - the step failed.  From the top of the enumeration
;--- again, ENUM_TRIES times; then the device is left on its port as
;--- USBS_DEAD, so that the port is not tried every tick for ever.  A
;--- pull and a push start it afresh.
usb_enum_fail:
IFDEF HANG_E9
        call    usb_trace_fail
ENDIF
        inc     byte ptr [ebx+USBD_TRIES]
        cmp     byte ptr [ebx+USBD_TRIES],ENUM_TRIES
        jae     uef_dead
        mov     byte ptr [ebx+USBD_SUB],ES_DEBOUNCE
        mov     byte ptr [ebx+USBD_FLAGS],0
        mov     byte ptr [ebx+USBD_SPEED],0
        call    ue_stamp
        jmp     ut_out
uef_dead:
        mov     edi,dword ptr [ebx+USBD_QH]
        or      edi,edi
        jz      uef_noq
        call    hc_intrdel
        mov     eax,dword ptr [ebx+USBD_RING]
        or      eax,eax
        jz      uef_noring
        call    usb_freering
        mov     dword ptr [ebx+USBD_RING],0
uef_noring:
        mov     eax,edi
        call    el_free
        mov     dword ptr [ebx+USBD_QH],0
uef_noq:
        mov     byte ptr [ebx+USBD_STATE],USBS_DEAD
        mov     byte ptr [usb_enumdev],0FFh
        jmp     ut_out

;---------------------------------------------------------------------
; usb_parsecfg - EDI -> a configuration descriptor and what follows it,
;                ECX bytes.  The first interface of a kind this kernel
;                knows is taken: boot keyboard, boot mouse, bulk-only
;                mass storage, hub.  Sets USBD_CLASS, USBD_IFACE and
;                the endpoints.  Preserves everything.
;---------------------------------------------------------------------
usb_parsecfg:
        pushad
        mov     esi,edi
        add     ecx,edi                 ; ECX = one past the end
        mov     byte ptr [ebx+USBD_IFACE],0
        xor     ebp,ebp                 ; EBP = 1 once an interface is chosen
        cmp     byte ptr [ebx+USBD_CLASS],USBC_HUB
        jne     upc_lp
        inc     ebp                     ; a hub by device class: its
                                        ; endpoints are not wanted
upc_lp:
        lea     eax,[esi+2]
        cmp     eax,ecx
        ja      upc_done
        movzx   edx,byte ptr [esi]      ; bLength
        or      edx,edx
        jz      upc_done
        lea     eax,[esi+edx]
        cmp     eax,ecx
        ja      upc_done
        mov     al,[esi+1]              ; bDescriptorType
        cmp     al,UDT_INTERFACE
        je      upc_iface
        cmp     al,UDT_ENDPOINT
        je      upc_ep
upc_next:
        add     esi,edx
        jmp     upc_lp

upc_iface:
        cmp     ebp,1
        je      upc_second              ; one is chosen: its endpoints
                                        ; are being collected, and the
                                        ; next interface ends that
        cmp     ebp,2
        je      upc_next
        cmp     edx,9
        jb      upc_next
        mov     al,[esi+5]              ; bInterfaceClass
        mov     ah,[esi+6]              ; bInterfaceSubClass
        mov     dl,[esi+7]              ; bInterfaceProtocol
        cmp     al,3                    ; HID
        jne     upc_nothid
        cmp     ah,1                    ; boot interface subclass
        jne     upc_skip
        cmp     dl,1
        je      upc_kbd
        cmp     dl,2
        je      upc_mouse
        jmp     upc_skip
upc_kbd:
        mov     byte ptr [ebx+USBD_CLASS],USBC_KBD
        jmp     upc_take
upc_mouse:
        mov     byte ptr [ebx+USBD_CLASS],USBC_MOUSE
        jmp     upc_take
upc_nothid:
        cmp     al,8                    ; mass storage
        jne     upc_nothub
        cmp     dl,50h                  ; bulk-only transport
        jne     upc_skip
        mov     byte ptr [ebx+USBD_CLASS],USBC_DISK
        jmp     upc_take
upc_nothub:
        cmp     al,9                    ; a hub by its interface
        jne     upc_skip
        mov     byte ptr [ebx+USBD_CLASS],USBC_HUB
        mov     al,[esi+2]
        mov     [ebx+USBD_IFACE],al
        mov     ebp,2
        movzx   edx,byte ptr [esi]
        jmp     upc_next
upc_take:
        mov     al,[esi+2]              ; bInterfaceNumber
        mov     [ebx+USBD_IFACE],al
        mov     ebp,1
        movzx   edx,byte ptr [esi]
        jmp     upc_next
upc_skip:
        movzx   edx,byte ptr [esi]
        jmp     upc_next
upc_second:
        mov     ebp,2
        movzx   edx,byte ptr [esi]
        jmp     upc_next

upc_ep:
        cmp     ebp,1
        jne     upc_next
        cmp     edx,7
        jb      upc_next
        movzx   edi,word ptr [esi+4]    ; wMaxPacketSize
        and     edi,7FFh
        jz      upc_next
        cmp     edi,64                  ; the most a full-speed one has
        ja      upc_next
        mov     al,[esi+3]              ; bmAttributes: transfer type
        and     al,3
        mov     ah,[esi+2]              ; bEndpointAddress
        cmp     byte ptr [ebx+USBD_CLASS],USBC_DISK
        je      upc_bulk
        ; HID: the first interrupt IN
        cmp     al,3
        jne     upc_next
        test    ah,80h
        jz      upc_next
        cmp     byte ptr [ebx+USBD_EPIN],0
        jne     upc_next
        and     ah,0Fh
        mov     [ebx+USBD_EPIN],ah
        mov     [ebx+USBD_MAXPIN],di
        jmp     upc_next
upc_bulk:
        cmp     al,2
        jne     upc_next
        test    ah,80h
        jz      upc_bulkout
        cmp     byte ptr [ebx+USBD_EPIN],0
        jne     upc_next
        and     ah,0Fh
        mov     [ebx+USBD_EPIN],ah
        mov     [ebx+USBD_MAXPIN],di
        jmp     upc_next
upc_bulkout:
        cmp     byte ptr [ebx+USBD_EPOUT],0
        jne     upc_next
        and     ah,0Fh
        mov     [ebx+USBD_EPOUT],ah
        mov     [ebx+USBD_MAXPOUT],di
        jmp     upc_next

upc_done:
        ; a class whose endpoints did not turn up is nothing to us
        mov     al,[ebx+USBD_CLASS]
        cmp     al,USBC_KBD
        je      upc_needin
        cmp     al,USBC_MOUSE
        je      upc_needin
        cmp     al,USBC_DISK
        jne     upc_out
        cmp     byte ptr [ebx+USBD_EPOUT],0
        je      upc_other
upc_needin:
        cmp     byte ptr [ebx+USBD_EPIN],0
        jne     upc_out
upc_other:
        mov     byte ptr [ebx+USBD_CLASS],USBC_OTHER
upc_out:
        popad
        ret

;=====================================================================
; HUBS.  A hub in service has its ports asked about one at a time,
; round the hubs and round the ports, one GET_PORT_STATUS a tick when
; nothing else is going on.  A connection change is cleared and acted
; on: a device on the port goes, a new one is enumerated.
;
; usb_hubpoll - CF if a job was begun.
;=====================================================================
usb_hubpoll:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        push    esi
        ; the next hub after usb_hubidx that is in service
        movzx   ecx,byte ptr [usb_hubidx]
        mov     edx,USB_MAXDEV
uhp_find:
        inc     ecx
        cmp     ecx,USB_MAXDEV
        jb      uhp_try
        xor     ecx,ecx
uhp_try:
        mov     al,cl
        call    dev_row
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     uhp_notthis
        cmp     byte ptr [ebx+USBD_CLASS],USBC_HUB
        je      uhp_found
uhp_notthis:
        dec     edx
        jnz     uhp_find
        jmp     uhp_none
uhp_found:
        mov     byte ptr [usb_hubidx],cl
        ; its next port
        mov     al,[ebx+USBD_HPORT]
        inc     al
        cmp     al,[ebx+USBD_UNIT]
        jbe     uhp_port
        mov     al,1
        or      byte ptr [ebx+USBD_FLAGS],USBDF_SWEPT
uhp_port:
        mov     [ebx+USBD_HPORT],al
        movzx   edx,al
        mov     al,0A3h
        mov     ah,UREQ_GETSTATUS
        xor     cx,cx
        mov     di,4
        call    usb_setup
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     ecx,4
        mov     byte ptr [usb_ctlown],1
        call    usb_ctl_start
        jc      uhp_nostart
        mov     byte ptr [usb_job],JOB_HUB
        mov     byte ptr [usb_hubstep],HJ_STATUS
        call    dev_slot                ; (CL was zeroed for wValue above)
        mov     byte ptr [usb_jobdev],al
        pop     esi
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
uhp_nostart:
        mov     byte ptr [usb_ctlown],0
uhp_none:
        pop     esi
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

;---------------------------------------------------------------------
; usb_hub_cont - the hub job's transfer has ended: AL = XS_*, ECX.
;---------------------------------------------------------------------
usb_hub_cont:
        mov     dl,al
        mov     al,byte ptr [usb_jobdev]
        call    dev_row
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     ut_out
        cmp     dl,XS_DONE
        jne     ut_out                  ; a hub that will not answer is
                                        ; asked again next time round
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_DESC
        mov     al,byte ptr [usb_hubstep]
        cmp     al,HJ_STATUS
        je      uhc_status
        cmp     al,HJ_CLEARCSC
        je      uhc_cleared
        jmp     ut_out

uhc_status:
        cmp     ecx,4
        jb      ut_out
        mov     ax,[edi+2]              ; wPortChange
        test    ax,HPC_CONNECT
        jnz     uhc_change
        ; no news.  A device registered on a port that reads empty is
        ; news all the same (a change this poll missed)
        mov     ax,[edi]
        test    ax,HPS_CONNECT
        jnz     ut_out
        call    dev_slot
        mov     ah,al
        mov     al,[ebx+USBD_HPORT]
        push    ebx
        call    usb_findhubport
        jc      uhc_nodev
        call    usb_detach
uhc_nodev:
        pop     ebx
        jmp     ut_out

uhc_change:
        ; CLEAR_FEATURE(C_PORT_CONNECTION), with the status remembered
        mov     ax,[edi]
        mov     word ptr [usb_hubstat],ax
        mov     byte ptr [usb_hubstep],HJ_CLEARCSC
        movzx   edx,byte ptr [ebx+USBD_HPORT]
        mov     al,23h
        mov     ah,UREQ_CLRFEAT
        mov     cx,HUBF_C_CONN
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        mov     byte ptr [usb_ctlown],1
        call    usb_ctl_start
        jc      uhc_nostart
        mov     byte ptr [usb_job],JOB_HUB
        jmp     ut_out
uhc_nostart:
        mov     byte ptr [usb_ctlown],0
        jmp     ut_out

uhc_cleared:
        ; whatever was on the port is gone; what is there now is new
        call    dev_slot
        mov     ah,al
        mov     al,[ebx+USBD_HPORT]
        push    ebx
        push    eax
        call    usb_findhubport
        jc      uhc_wasempty
        call    usb_detach
uhc_wasempty:
        pop     eax
        pop     ebx
        test    word ptr [usb_hubstat],HPS_CONNECT
        jz      ut_out
        or      ah,80h                  ; a hub's port, usb_enum_begin's way
        call    usb_enum_begin
        jmp     ut_out

;=====================================================================
; PARKING, AND A RESTART.
;
; usb_park - AL = 0: the machine is about to be handed to real mode.
;            Every controller is stopped and its registers put back as
;            the BIOS left them, legacy emulation included, so that
;            whatever runs down there finds what it would have found.
;            AL = 1: it has come back - the controllers are taken
;            again and every port looked at afresh.  Everything
;            preserved.
; usb_halt - the controllers stopped and reset, for a restart through
;            the ROM: a schedule left running would be writing into
;            memory the BIOS is about to use.
;=====================================================================
usb_park:
        pushad
        cmp     byte ptr [usb_hcn],0
        je      upk_out
        or      al,al
        jnz     upk_up
        cmp     byte ptr [usb_kalive],0
        je      upk_teardown            ; native: hand the controllers back
        ; THE WfW BRIDGE KEEPS USB OWNED across the park: the controllers
        ; stay owned, every device stays enumerated, and the frame list and
        ; queues stay exactly as built, so the bridge (PMDOS.386 ->
        ; blk_bridge) can read a stick's sectors while Windows owns the
        ; machine - the same way the NIC is kept awake for the parked stack.
        ; Nothing is handed to the BIOS; a reboot still resets the
        ; controller through usb_halt, so the "schedule into BIOS memory"
        ; hazard is covered.  Built only for the WfW-bridge rig
        ; (-d USB_KEEPALIVE); the default build hands the controllers back
        ; as it always has (BC2's opt-in is to become a CONFIG USB= option).
        ;
        ; The SCHEDULE IS PAUSED here (Run/Stop cleared, the controller
        ; halted) so it bus-masters NOTHING while Windows runs its GUI
        ; init - the bridge wakes it for the span of each transfer
        ; (blk_bridge: usb_kresume ... usb_kpause) and pauses it again.
        ; usb_started stays 1: the stick is still mounted and readable,
        ; only the hardware schedule rests.  (Decoupling the live schedule
        ; from the park - direction A of the BC2 boot-blocker chase.)
        call    usb_kpause
        popad
        ret
upk_teardown:
        cmp     byte ptr [usb_started],0
        je      upk_out
        mov     byte ptr [usb_started],0
        ; every device goes: the tables are rebuilt on the way back,
        ; and a disk's letter is told so by the unit's generation
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
upk_dev:
        call    usb_detach
        add     ebx,USBD_LEN
        loop    upk_dev
        call    usb_reap
        mov     byte ptr [usb_job],JOB_NONE
        mov     byte ptr [usb_ctlown],0
        mov     byte ptr [usb_enumdev],0FFh
        mov     byte ptr [usb_cxbusy],0
        mov     byte ptr [usb_bxbusy],0
        xor     ecx,ecx
upk_hc:
        cmp     cl,[usb_hcn]
        jae     upk_parked
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        test    byte ptr [esi+HC_FLAGS],USBHF_OWNED
        jz      upk_hcnext
        call    hc_handback
        and     byte ptr [esi+HC_FLAGS],NOT USBHF_OWNED
upk_hcnext:
        inc     ecx
        jmp     upk_hc
upk_parked:
        mov     byte ptr [usb_parked],1
        jmp     upk_out

upk_up:
        cmp     byte ptr [usb_kalive],0
        je      upk_retakeall           ; native: re-take the controllers afresh
        ; the keepalive never tore anything down at the park - it only
        ; paused the schedule - so coming back just resumes it; the
        ; foreground kernel's own tick drives USB from here as before.
        call    usb_kresume
        popad
        ret
upk_retakeall:
        cmp     byte ptr [usb_parked],0
        je      upk_out
        mov     byte ptr [usb_parked],0
        ; the pool afresh - nothing of the old schedule is wanted -
        ; and the controllers taken again
        call    usb_carve
        xor     ecx,ecx
upk_retake:
        cmp     cl,[usb_hcn]
        jae     upk_retaken
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        cmp     byte ptr [esi+HC_KIND],USBK_EHCI
        je      upk_rnext
        call    hc_init
        jc      upk_rnext
        or      byte ptr [esi+HC_FLAGS],USBHF_OWNED
upk_rnext:
        inc     ecx
        jmp     upk_retake
upk_retaken:
        mov     byte ptr [usb_forcescan],1
        mov     byte ptr [usb_started],1
upk_out:
        popad
        ret

usb_halt:
        pushad
        cmp     byte ptr [usb_started],0
        je      uha_out
        mov     byte ptr [usb_started],0
        xor     ecx,ecx
uha_hc:
        cmp     cl,[usb_hcn]
        jae     uha_out
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        test    byte ptr [esi+HC_FLAGS],USBHF_OWNED
        jz      uha_next
        call    hc_halt
uha_next:
        inc     ecx
        jmp     uha_hc
uha_out:
        popad
        ret

;=====================================================================
; usb_kpause / usb_kresume - the WfW-bridge keepalive's schedule gate.
; Stop / run every OWNED UHCI's schedule, keeping every register and the
; built frame list and queues.  usb_kpause runs at the park and after
; each bridge transfer; usb_kresume runs on the way back and before each
; transfer.  UHCI only: the rig's stick is on a PIIX3 UHCI; an OHCI (the
; rig has none) is left as it was.  usb_started is never touched.
;
; The HANG_E9 traces make one rig run tell where a transfer stops: 'Q' at
; a pause, then per transfer 'R' <CMD> <STS> at the resume - CMD's low
; byte should read C1 (Run|ConfigFlag|MaxPacket) and STS's should clear
; its 20h HALTED bit, proving the schedule actually restarted.
;=====================================================================
usb_kpause:
        pushad
IFDEF HANG_E9
        mov     al,'Q'
        out     0E9h,al
ENDIF
        xor     ecx,ecx
ukp_hc:
        cmp     cl,[usb_hcn]
        jae     ukp_out
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        test    byte ptr [esi+HC_FLAGS],USBHF_OWNED
        jz      ukp_next
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        jne     ukp_next
        call    uh_stoprun
ukp_next:
        inc     ecx
        jmp     ukp_hc
ukp_out:
        popad
        ret

usb_kresume:
        pushad
        xor     ecx,ecx
ukr_hc:
        cmp     cl,[usb_hcn]
        jae     ukr_out
        imul    esi,ecx,HC_LEN
        add     esi,OFFSET usb_hcs
        test    byte ptr [esi+HC_FLAGS],USBHF_OWNED
        jz      ukr_next
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        jne     ukr_next
        call    uh_runsched
IFDEF HANG_E9
        mov     al,'R'
        out     0E9h,al
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_CMD
        in      ax,dx
        movzx   eax,ax
        call    e9_hex8                 ; CMD: low byte C1 = Run|CF|MaxP
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_STS
        in      ax,dx
        movzx   eax,ax
        call    e9_hex8                 ; STS: 20h HALTED should be gone
ENDIF
ukr_next:
        inc     ecx
        jmp     ukr_hc
ukr_out:
        popad
        ret

;=====================================================================
; EHCI.  Not driven: a USB 2.0 controller takes every full-speed and
; high-speed device on its ports for itself, and hands them down to
; the UHCI or OHCI companion beside it only while its CONFIGFLAG is
; clear - which the BIOS, if it has legacy USB support on, has set.
; So the BIOS is asked to let go through the extended capability's
; semaphore, the controller is stopped and reset, and the flag that a
; reset leaves clear stays clear.  Every stick then enumerates on the
; companion at full speed, which is what this kernel can drive.
;
; ehci_handdown - ESI -> the EHCI's row.  Preserves everything.
;=====================================================================
ehci_handdown:
        pushad
        mov     ebx,dword ptr [esi+HC_PCI]
        xor     dl,dl
        call    pcx_bar                 ; BAR0 -> EAX = base, EDX = flags
        jc      ehd_out
        test    edx,BARF_IO OR BARF_NONE
        jnz     ehd_out
        mov     ebx,eax
        mov     ecx,1000h
        call    pg_mapphys              ; -> EAX = where it can be read
        jc      ehd_out
        mov     edi,eax                 ; EDI = the capability registers
        mov     dword ptr [esi+HC_BASE],eax
        mov     ebx,dword ptr [esi+HC_PCI]

        ; the legacy capability, if it has one: USBLEGSUP at EECP
        mov     eax,dword ptr [edi+08h] ; HCCPARAMS
        shr     eax,8
        movzx   ecx,al                  ; EECP
        mov     [esi+HC_EECP],cl
        cmp     ecx,40h
        jb      ehd_stop
        cmp     ecx,0F8h                ; both its words inside the header
        ja      ehd_stop
        ; BIOS owned (bit 16)?  Ask for it (bit 24) and wait
        mov     al,cl
        call    pci_read
        test    eax,00010000h
        jz      ehd_owned
        or      eax,01000000h
        push    ecx
        mov     ecx,eax
        mov     al,[esi+HC_EECP]
        call    pci_write
        pop     ecx
        push    ecx
        mov     ecx,19                  ; a second
        push    ebx
        mov     ebx,dword ptr ds:[BDA_TICKS]
ehd_wait:
        sti
        mov     al,[esi+HC_EECP]
        push    ebx
        mov     ebx,dword ptr [esi+HC_PCI]
        call    pci_read
        pop     ebx
        test    eax,00010000h
        jz      ehd_waited
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,ebx
        cmp     eax,ecx
        jb      ehd_wait
ehd_waited:
        pop     ebx
        pop     ecx
ehd_owned:
        ; no SMIs from it, whatever the BIOS had asked for
        mov     al,cl
        add     al,4
        push    eax
        call    pci_read
        mov     dword ptr [esi+HC_SMIWAS],eax
        pop     eax
        push    ecx
        xor     ecx,ecx
        call    pci_write
        pop     ecx

ehd_stop:
        ; the operational registers: stop, reset, and the flag is clear
        movzx   eax,byte ptr [edi]      ; CAPLENGTH
        add     edi,eax
        mov     eax,dword ptr [edi]     ; USBCMD
        and     eax,NOT 1               ; run/stop off
        mov     dword ptr [edi],eax
        mov     ecx,2000
ehd_halt:
        test    dword ptr [edi+4],1000h ; USBSTS.HCHalted
        jnz     ehd_halted
        push    ecx
        mov     ecx,10
        call    usb_udelay
        pop     ecx
        loop    ehd_halt
ehd_halted:
        or      dword ptr [edi],2       ; HCRESET
        mov     ecx,2000
ehd_rst:
        test    dword ptr [edi],2
        jz      ehd_reset
        push    ecx
        mov     ecx,10
        call    usb_udelay
        pop     ecx
        loop    ehd_rst
ehd_reset:
        mov     dword ptr [edi+40h],0   ; CONFIGFLAG: the companions' ports
        or      byte ptr [esi+HC_FLAGS],USBHF_OWNED
ehd_out:
        popad
        ret

;=====================================================================
; State
;=====================================================================
usb_hcn     db  0                       ; controllers in usb_hcs
usb_ehcin   db  0                       ; ...of which EHCI
usb_started db  0                       ; the tick is driving them
usb_kalive  db  0                       ; CONFIG.SYS USB=ON,KEEPALIVE: keep the
                                        ; controllers owned + the stick enumerated
                                        ; across the park so the WfW bridge can
                                        ; read it (set via IOS_USB USF_KEEPALIVE;
                                        ; 0 = hand USB back at the park, native)
usb_brgwin  db  0                       ; a bridged USB transfer is running
                                        ; (set by blk_bridge): the transfer
                                        ; poll must NOT sti, because the bridge
                                        ; window runs under the kernel's CR3
                                        ; with WIN386's IDT out of reach - an
                                        ; interrupt delivered here triple-faults
usb_parked  db  0                       ; handed back for a park
usb_forcescan db 0                      ; every connected port is new
usb_job     db  0                       ; JOB_*
usb_jobdev  db  0                       ; the slot it is about
usb_enumdev db  0FFh                    ; the slot being enumerated
usb_ctlown  db  0                       ; 0 free, 1 the tick, 2 the foreground
usb_hubidx  db  0                       ; the hub last polled
usb_hubstep db  0                       ; HJ_*
usb_hubstat dw  0                       ; wPortStatus, for the step after
usb_mem     dd  0                       ; the block PMDOS bought
usb_scr     dd  0                       ; its scratch area
usb_poolfree dd 0                       ; the free list
usb_nelem   dd  0

; the stage builder's
usb_mkfirst dd  0
usb_mklast  dd  0
usb_mkleft  dd  0                       ; bytes still to describe
usb_mkbuf   dd  0                       ; ...from here
usb_mkps    dw  0                       ; the endpoint's packet size
usb_mkpid   dw  0                       ; PID:endpoint
usb_mktog   db  0
usb_mkflags db  0
            db  0,0

; the poll's
usb_cpticks dd  0
usb_cpstart dd  0
usb_cpstatus dd 0                       ; the status stage's descriptor, or 0

; the control transfer
usb_cxdev   dd  0
usb_cxfirst dd  0
usb_cxlast  dd  0
usb_cxbuf   dd  0
usb_cxlen   dd  0
usb_cxtime  dd  0
usb_cxbusy  db  0
usb_cxin    db  0                       ; bmRequestType: bit 7 for IN
            db  0,0
usb_setupbuf db 8 dup(0)

; the bulk transfer
usb_bxdev   dd  0
usb_bxfirst dd  0
usb_bxlen   dd  0
usb_bxtime  dd  0
usb_bxbusy  db  0
usb_bxdir   db  0
usb_bxtog0  db  0
usb_bxtogend db 0

IFDEF HANG_E9
usl_spins   dd  0
usb_wflag   db  0
msgUsl      db  'USL ',0
ENDIF
IFDEF HANG_E9
;--- a control chain handed over: R <cmd> <sts> <frnum> then D<4 dwords> each
usb_trace_run:
        pushad
        cmp     byte ptr [esi+HC_KIND],USBK_UHCI
        jne     utr_out
        mov     al,'R'
        out     0E9h,al
        mov     edx,dword ptr [esi+HC_BASE]
        in      ax,dx
        movzx   eax,ax
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        add     dx,2
        in      ax,dx
        movzx   eax,ax
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        add     dx,4
        in      ax,dx
        movzx   eax,ax
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [esi+HC_CTLQ]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [esi+HC_CTLQ]
        mov     eax,dword ptr [eax+EL_HW1]
        call    e9_hex32
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
        mov     ebp,dword ptr [usb_cxfirst]
utr_td:
        or      ebp,ebp
        jz      utr_out
        mov     al,'D'
        out     0E9h,al
        mov     eax,ebp
        call    e9_hex32
        mov     al,':'
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW0]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW1]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW2]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [ebp+EL_HW3]
        call    e9_hex32
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
        mov     ebp,dword ptr [ebp+EL_NEXT]
        jmp     utr_td
utr_out:
        popad
        ret
ENDIF
IFDEF HANG_E9
;--- the enumeration, on port E9h: C<sub> <al><ah> <ecx> and F<sub>
usb_trace_cont:
        pushad
        mov     al,'C'
        out     0E9h,al
        movzx   eax,byte ptr [ebx+USBD_SUB]
        call    e9_hex8
        mov     al,' '
        out     0E9h,al
        movzx   eax,dl
        call    e9_hex8
        movzx   eax,dh
        call    e9_hex8
        mov     al,' '
        out     0E9h,al
        mov     eax,ecx
        call    e9_hex32
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
        popad
        ret
usb_trace_fail:
        pushad
        mov     al,'F'
        out     0E9h,al
        movzx   eax,byte ptr [ebx+USBD_SUB]
        call    e9_hex8
        mov     al,' '
        out     0E9h,al
        mov     cl,[ebx+USBD_PORT]
        call    hc_portst
        movzx   eax,al
        call    e9_hex8
        mov     al,' '
        out     0E9h,al
        mov     edx,dword ptr [esi+HC_BASE]
        in      ax,dx                   ; USBCMD
        movzx   eax,ax
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        add     dx,2
        in      ax,dx                   ; USBSTS
        movzx   eax,ax
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        add     dx,4
        in      ax,dx                   ; FRNUM
        movzx   eax,ax
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [esi+HC_CTLQ]
        mov     eax,dword ptr [eax+EL_HW1]      ; the control queue's element
        call    e9_hex32
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
        popad
        ret
ENDIF
usb_hcs     db  USB_MAXHC * HC_LEN dup(0)
usb_devs    db  USB_MAXDEV * USBD_LEN dup(0)
