;=====================================================================
; OHCI.INC - the Open Host Controller: SiS, ALi, NEC, Compaq, Apple,
;            and the CMD and Opti cards, memory mapped
;
; Where UHCI gives the driver a frame list and a packet-sized
; descriptor, OHCI gives it ENDPOINT DESCRIPTORS - one per endpoint,
; holding the address, the packet size and the speed once - each with
; a queue of transfer descriptors that carry up to eight kilobytes
; apiece across two pages.  The controller keeps three lists of them:
; control, bulk, and the periodic list it reaches through a 32-entry
; table in the HCCA, the communication area it also writes its frame
; number and done queue into.
;
; An ED's queue runs from HeadP to TailP and the descriptor AT TailP
; is never executed: it is a placeholder, and adding a transfer means
; filling the placeholder and hanging a fresh one behind it.  The
; control and bulk EDs here hold one chain at a time and are
; re-targeted at a device and endpoint for each; a HID device has an
; ED of its own in the periodic table, with a ring of descriptors
; that rotates through the placeholder role (oh_rearm).
;
; The controller retires descriptors onto a done queue and says so
; with WDH.  Nothing here reads the queue - the status is in the
; descriptor itself - but WDH is written off every tick so that the
; controller goes on handing them back.  No interrupt is enabled.
;
; The OHCI 1.0a specification is the reference; Linux's ohci-hcd for
; the BIOS hand-over.  Tested on QEMU's pci-ohci.
;
; Included by IOPM.ASM after UHCI.INC.
;=====================================================================

;--- the registers, from the mapped base -----------------------------
OHR_REVISION    EQU     00h
OHR_CONTROL     EQU     04h
OHR_CMDSTATUS   EQU     08h
OHR_INTSTATUS   EQU     0Ch
OHR_INTENABLE   EQU     10h
OHR_INTDISABLE  EQU     14h
OHR_HCCA        EQU     18h
OHR_PERIODCUR   EQU     1Ch
OHR_CTLHEAD     EQU     20h
OHR_CTLCUR      EQU     24h
OHR_BLKHEAD     EQU     28h
OHR_BLKCUR      EQU     2Ch
OHR_DONEHEAD    EQU     30h
OHR_FMINTERVAL  EQU     34h
OHR_FMREMAIN    EQU     38h
OHR_FMNUMBER    EQU     3Ch
OHR_PERSTART    EQU     40h
OHR_LSTHRESH    EQU     44h
OHR_RHDESCA     EQU     48h
OHR_RHDESCB     EQU     4Ch
OHR_RHSTATUS    EQU     50h
OHR_RHPORT      EQU     54h             ; one a port, from port 1

OHC_CBSR3       EQU     00000003h       ; four control EDs a bulk one
OHC_PLE         EQU     00000004h       ; periodic list on
OHC_IE          EQU     00000008h       ; isochronous (not used)
OHC_CLE         EQU     00000010h       ; control list on
OHC_BLE         EQU     00000020h       ; bulk list on
OHC_HCFS        EQU     000000C0h       ; the functional state...
OHC_HCFS_RESET  EQU     00000000h
OHC_HCFS_RESUME EQU     00000040h
OHC_HCFS_OPER   EQU     00000080h
OHC_HCFS_SUSP   EQU     000000C0h
OHC_IR          EQU     00000100h       ; interrupts go to SMM: the BIOS owns it
OHC_RUNNING     EQU     OHC_CBSR3 OR OHC_PLE OR OHC_CLE OR OHC_BLE OR OHC_HCFS_OPER

OHS_HCR         EQU     00000001h       ; reset
OHS_CLF         EQU     00000002h       ; control list filled
OHS_BLF         EQU     00000004h       ; bulk list filled
OHS_OCR         EQU     00000008h       ; ownership change request

OHI_WDH         EQU     00000002h       ; writeback done head
OHI_ALL         EQU     0C000007Fh

OHRH_LPSC       EQU     00010000h       ; RhStatus: set global power
OHRP_CCS        EQU     00000001h       ; connected
OHRP_PES        EQU     00000002h       ; enabled
OHRP_PRS        EQU     00000010h       ; write: reset the port
OHRP_PPS        EQU     00000100h       ; write: power the port
OHRP_LSDA       EQU     00000200h       ; a low-speed device
OHRP_CSC        EQU     00010000h       ; change bits, write 1 to clear
OHRP_PESC       EQU     00020000h
OHRP_PSSC       EQU     00040000h
OHRP_OCIC       EQU     00080000h
OHRP_PRSC       EQU     00100000h       ; the reset is over
OHRP_CHANGES    EQU     001F0000h

OH_FMINTERVAL   EQU     27782EDFh       ; 11999 bit times, 10104 of them
                                        ; for the largest packet
OH_PERSTART     EQU     00002A2Fh       ; the periodic list from 90%

;--- an endpoint descriptor's first dword ----------------------------
OHE_FA          EQU     0000007Fh       ; function address
OHE_EN_SHIFT    EQU     7               ; endpoint
OHE_D_SHIFT     EQU     11              ; direction: 0 from the TD, 1 OUT,
OHE_D_OUT       EQU     00000800h       ; 2 IN
OHE_D_IN        EQU     00001000h
OHE_S           EQU     00002000h       ; low speed
OHE_K           EQU     00004000h       ; skip
OHE_F           EQU     00008000h       ; isochronous (not used)
OHE_MPS_SHIFT   EQU     16
OHE_HALT        EQU     1               ; in HeadP: halted
OHE_CARRY       EQU     2               ; ...and the toggle carry

;--- a transfer descriptor's first dword -----------------------------
OHT_R           EQU     00040000h       ; a short packet is not an error
OHT_DP_SETUP    EQU     00000000h
OHT_DP_OUT      EQU     00080000h
OHT_DP_IN       EQU     00100000h
OHT_DI_NONE     EQU     00E00000h       ; no interrupt
OHT_T_CARRY     EQU     00000000h       ; the toggle from the ED
OHT_T_DATA0     EQU     02000000h
OHT_T_DATA1     EQU     03000000h
OHT_CC_SHIFT    EQU     28
OHT_CC_FRESH    EQU     0F0000000h      ; not accessed
OHT_FRESH       EQU     OHT_CC_FRESH OR OHT_DI_NONE

; condition codes
OHCC_OK         EQU     0
OHCC_CRC        EQU     1
OHCC_BITSTUFF   EQU     2
OHCC_TOGGLE     EQU     3
OHCC_STALL      EQU     4
OHCC_NOANSWER   EQU     5
OHCC_PIDCHECK   EQU     6
OHCC_BADPID     EQU     7
OHCC_OVERRUN    EQU     8
OHCC_UNDERRUN   EQU     9
OHCC_FRESH      EQU     0Fh
OHCC_FRESH2     EQU     0Eh

HCCA_INTTAB     EQU     00h             ; 32 dwords
HCCA_FRAME      EQU     80h             ; dw
HCCA_DONEHEAD   EQU     84h             ; dd

;=====================================================================
; oh_init - ESI -> the controller's row: taken from the BIOS, reset
;           and running.  CF if its registers could not be mapped.
;=====================================================================
oh_init:
        pushad
        mov     ebx,dword ptr [esi+HC_PCI]
        xor     dl,dl                   ; BAR0: the registers, 4K of memory
        call    pcx_bar
        jc      ohi_fail
        test    edx,BARF_IO OR BARF_NONE
        jnz     ohi_fail
        mov     ebx,eax
        mov     ecx,1000h
        call    pg_mapphys
        jc      ohi_fail
        mov     dword ptr [esi+HC_BASE],eax
        mov     edi,eax                 ; EDI -> the registers throughout
        mov     ebx,dword ptr [esi+HC_PCI]
        mov     al,PCIR_COMMAND
        call    pci_read
        mov     ecx,eax
        and     ecx,0000FFFFh
        or      ecx,PCIC_MEMEN OR PCIC_BUSMASTER
        mov     al,PCIR_COMMAND
        call    pci_write

        ; as found, for the hand-back
        mov     eax,dword ptr [edi+OHR_CONTROL]
        mov     dword ptr [esi+HC_SAVE+0],eax
        mov     eax,dword ptr [edi+OHR_FMINTERVAL]
        mov     dword ptr [esi+HC_SAVE+4],eax
        mov     eax,dword ptr [edi+OHR_HCCA]
        mov     dword ptr [esi+HC_SAVE+8],eax
        mov     eax,dword ptr [edi+OHR_CTLHEAD]
        mov     dword ptr [esi+HC_SAVE+12],eax
        mov     eax,dword ptr [edi+OHR_BLKHEAD]
        mov     dword ptr [esi+HC_SMIWAS],eax

        ; the BIOS's: ask, and give it half a second to let go
        test    dword ptr [edi+OHR_CONTROL],OHC_IR
        jz      ohi_notbios
        or      byte ptr [esi+HC_FLAGS],USBHF_LEGACY
        or      dword ptr [edi+OHR_CMDSTATUS],OHS_OCR
        mov     ecx,9
ohi_ocr:
        push    ecx
        mov     ecx,1
        call    usb_waitticks
        pop     ecx
        test    dword ptr [edi+OHR_CONTROL],OHC_IR
        jz      ohi_notbios
        loop    ohi_ocr
ohi_notbios:
        ; interrupts off before the reset, so that nothing it did can
        ; reach the SMM handler on its way out
        mov     dword ptr [edi+OHR_INTDISABLE],OHI_ALL

        ; the reset: ten microseconds, and the chip clears the bit
        mov     dword ptr [edi+OHR_CMDSTATUS],OHS_HCR
        mov     ecx,100
ohi_rst:
        test    dword ptr [edi+OHR_CMDSTATUS],OHS_HCR
        jz      ohi_reset
        push    ecx
        mov     ecx,10
        call    usb_udelay
        pop     ecx
        loop    ohi_rst
        jmp     ohi_fail
ohi_reset:
        ; the skeleton: a control ED and a bulk ED, each with its
        ; placeholder, skipped until given a chain
        call    el_alloc
        jc      ohi_fail
        mov     dword ptr [esi+HC_CTLDUMMY],eax
        call    el_alloc
        jc      ohi_fail
        mov     dword ptr [esi+HC_BLKDUMMY],eax
        call    el_alloc
        jc      ohi_fail
        mov     dword ptr [esi+HC_CTLQ],eax
        mov     dword ptr [eax+EL_HW0],OHE_K
        mov     edx,dword ptr [esi+HC_CTLDUMMY]
        mov     dword ptr [eax+EL_HW1],edx
        mov     dword ptr [eax+EL_HW2],edx
        mov     dword ptr [eax+EL_HW3],0
        call    el_alloc
        jc      ohi_fail
        mov     dword ptr [esi+HC_BLKQ],eax
        mov     dword ptr [eax+EL_HW0],OHE_K
        mov     edx,dword ptr [esi+HC_BLKDUMMY]
        mov     dword ptr [eax+EL_HW1],edx
        mov     dword ptr [eax+EL_HW2],edx
        mov     dword ptr [eax+EL_HW3],0
        mov     dword ptr [esi+HC_INTHEAD],0

        ; the registers, in the order the specification gives: the
        ; frame interval the reset wiped, the lists, the HCCA, and then
        ; operational - within a couple of milliseconds of the reset,
        ; or the root hub thinks it is being suspended
        mov     dword ptr [edi+OHR_FMINTERVAL],OH_FMINTERVAL
        mov     dword ptr [edi+OHR_PERSTART],OH_PERSTART
        mov     eax,dword ptr [esi+HC_FL]
        mov     dword ptr [edi+OHR_HCCA],eax
        mov     eax,dword ptr [esi+HC_CTLQ]
        mov     dword ptr [edi+OHR_CTLHEAD],eax
        mov     dword ptr [edi+OHR_CTLCUR],0
        mov     eax,dword ptr [esi+HC_BLKQ]
        mov     dword ptr [edi+OHR_BLKHEAD],eax
        mov     dword ptr [edi+OHR_BLKCUR],0
        mov     dword ptr [edi+OHR_INTSTATUS],OHI_ALL
        mov     dword ptr [edi+OHR_INTDISABLE],OHI_ALL
        mov     dword ptr [edi+OHR_CONTROL],OHC_RUNNING

        ; the root hub: how many ports, and power to all of them
        mov     eax,dword ptr [edi+OHR_RHDESCA]
        mov     edx,eax
        and     al,0Fh
        jnz     ohi_haveports
        mov     al,2
ohi_haveports:
        mov     [esi+HC_NPORTS],al
        mov     dword ptr [edi+OHR_RHSTATUS],OHRH_LPSC
        movzx   ecx,al
        lea     ebx,[edi+OHR_RHPORT]
ohi_power:
        mov     dword ptr [ebx],OHRP_PPS
        add     ebx,4
        loop    ohi_power
        ; and the time they take to come up: POTPGT, in 2ms units,
        ; rounded up to ticks, plus one
        shr     edx,24
        lea     eax,[edx*2+54]
        xor     edx,edx
        mov     ecx,55
        div     ecx
        lea     ecx,[eax+1]
        call    usb_waitticks
        popad
        clc
        ret
ohi_fail:
        popad
        stc
        ret

;=====================================================================
; oh_handback - stopped, and the registers as the BIOS left them.  If
;               the BIOS had it, InterruptRouting goes back with the
;               rest and its SMM code finds its own lists where it
;               left them.
;=====================================================================
oh_handback:
        pushad
        mov     edi,dword ptr [esi+HC_BASE]
        mov     dword ptr [edi+OHR_INTDISABLE],OHI_ALL
        mov     dword ptr [edi+OHR_CMDSTATUS],OHS_HCR
        mov     ecx,100
ohh_rst:
        test    dword ptr [edi+OHR_CMDSTATUS],OHS_HCR
        jz      ohh_reset
        push    ecx
        mov     ecx,10
        call    usb_udelay
        pop     ecx
        loop    ohh_rst
ohh_reset:
        mov     eax,dword ptr [esi+HC_SAVE+4]
        mov     dword ptr [edi+OHR_FMINTERVAL],eax
        mov     dword ptr [edi+OHR_PERSTART],OH_PERSTART
        mov     eax,dword ptr [esi+HC_SAVE+8]
        mov     dword ptr [edi+OHR_HCCA],eax
        mov     eax,dword ptr [esi+HC_SAVE+12]
        mov     dword ptr [edi+OHR_CTLHEAD],eax
        mov     dword ptr [edi+OHR_CTLCUR],0
        mov     eax,dword ptr [esi+HC_SMIWAS]
        mov     dword ptr [edi+OHR_BLKHEAD],eax
        mov     dword ptr [edi+OHR_BLKCUR],0
        mov     eax,dword ptr [esi+HC_SAVE+0]
        mov     dword ptr [edi+OHR_CONTROL],eax
        popad
        ret

;=====================================================================
; oh_halt - into the reset state, for a restart of the machine.
;=====================================================================
oh_halt:
        push    edi
        mov     edi,dword ptr [esi+HC_BASE]
        mov     dword ptr [edi+OHR_INTDISABLE],OHI_ALL
        mov     dword ptr [edi+OHR_CONTROL],OHC_HCFS_RESET
        mov     dword ptr [edi+OHR_CMDSTATUS],OHS_HCR
        pop     edi
        ret

;=====================================================================
; The ports.  CL = the port, from 1.
;=====================================================================
oh_portreg:                             ; -> EDX -> its register
        mov     edx,dword ptr [esi+HC_BASE]
        add     edx,OHR_RHPORT - 4
        push    ecx
        movzx   ecx,cl
        shl     ecx,2
        add     edx,ecx
        pop     ecx
        ret

oh_portst:
        push    edx
        call    oh_portreg
        mov     edx,dword ptr [edx]
        xor     al,al
        test    edx,OHRP_CCS
        jz      ops_nocon
        or      al,PST_CONNECT
ops_nocon:
        test    edx,OHRP_CSC
        jz      ops_nochg
        or      al,PST_CHANGE
ops_nochg:
        test    edx,OHRP_PES
        jz      ops_nope
        or      al,PST_ENABLED
ops_nope:
        test    edx,OHRP_LSDA
        jz      ops_fs
        or      al,PST_LOWSPEED
ops_fs:
        test    edx,OHRP_PRSC
        jz      ops_out
        or      al,PST_RESETDONE
ops_out:
        pop     edx
        ret

oh_portack:
        push    edx
        call    oh_portreg
        mov     dword ptr [edx],OHRP_CHANGES
        pop     edx
        ret

oh_portreset:
        push    edx
        call    oh_portreg
        mov     dword ptr [edx],OHRP_PRS
        pop     edx
        ret

oh_portenable:                          ; the reset enables the port
        ret

;=====================================================================
; Descriptors.
;=====================================================================

;---------------------------------------------------------------------
; oh_mktd - EBX -> device, DL = PID, DH = endpoint (unused: the ED has
;           it), EDI = buffer, ECX = bytes, AL = toggle, AH = ELF_* ->
;           EAX = the element.  CF if the pool is empty.
;
; The toggle is written into the descriptor only where a stage begins
; and the stage is not a bulk one; everything else takes the ED's
; carry, which the controller keeps right across descriptors.
;---------------------------------------------------------------------
oh_mktd:
        pushad
        mov     byte ptr [oh_tpid],dl
        mov     byte ptr [oh_ttog],al
        mov     byte ptr [oh_tflags],ah
        mov     dword ptr [oh_tbuf],edi
        mov     dword ptr [oh_tlen],ecx
        call    el_alloc
        jc      omt_fail
        mov     ebp,eax
        mov     eax,dword ptr [oh_tbuf]
        mov     dword ptr [ebp+EL_BUF],eax
        mov     ecx,dword ptr [oh_tlen]
        mov     word ptr [ebp+EL_LEN16],cx
        mov     al,byte ptr [oh_tflags]
        mov     [ebp+EL_FLAGS],al
        mov     al,byte ptr [oh_ttog]
        mov     [ebp+EL_TOG],al
        ; dword 0
        mov     edx,OHT_FRESH
        mov     al,byte ptr [oh_tpid]
        cmp     al,PID_SETUP
        je      omt_dp
        or      edx,OHT_DP_OUT
        cmp     al,PID_OUT
        je      omt_dp
        mov     edx,OHT_FRESH OR OHT_DP_IN OR OHT_R
omt_dp:
        test    byte ptr [oh_tflags],ELF_BULK
        jnz     omt_carry
        test    byte ptr [oh_tflags],ELF_FIRST
        jz      omt_carry
        or      edx,OHT_T_DATA0
        test    byte ptr [oh_ttog],1
        jz      omt_carry
        or      edx,OHT_T_DATA1
omt_carry:
        mov     dword ptr [ebp+EL_HW0],edx
        ; the buffer: CBP is the first byte and BE the last, and both
        ; are zero for a packet with no data
        mov     eax,dword ptr [oh_tbuf]
        or      ecx,ecx
        jnz     omt_data
        xor     eax,eax
        mov     dword ptr [ebp+EL_HW1],eax
        mov     dword ptr [ebp+EL_HW3],eax
        jmp     omt_linked
omt_data:
        mov     dword ptr [ebp+EL_HW1],eax
        add     eax,ecx
        dec     eax
        mov     dword ptr [ebp+EL_HW3],eax
omt_linked:
        mov     dword ptr [ebp+EL_HW2],0
        mov     [esp+28],ebp
        popad
        clc
        ret
omt_fail:
        popad
        stc
        ret

;--- oh_tdmax - a descriptor reaches to the end of the page after the
;--- one it starts in: ECX = min(ECX, 8192 - (EDI & 0FFFh))
oh_tdmax:
        push    eax
        mov     eax,edi
        and     eax,0FFFh
        neg     eax
        add     eax,2000h
        cmp     ecx,eax
        jbe     otm_out
        mov     ecx,eax
otm_out:
        pop     eax
        ret

;--- oh_link - EAX's NextTD is EDX
oh_link:
        mov     dword ptr [eax+EL_HW2],edx
        ret

;--- oh_term - the last of a chain points at nothing, for now: oh_run
;--- hangs the placeholder behind it
oh_term:
        mov     dword ptr [eax+EL_HW2],0
        ret

;---------------------------------------------------------------------
; oh_run - EDI = the control or bulk ED, EAX = the chain's first
;          descriptor, EBX -> device, CL = endpoint (bit 7 for IN), CH
;          = the toggle to carry from, DX = packet size.  The ED is
;          aimed at the endpoint and the chain hung on it.
;---------------------------------------------------------------------
oh_run:
        pushad
        mov     ebp,eax                 ; EBP = the first descriptor
        or      dword ptr [edi+EL_HW0],OHE_K
        ; which placeholder, by which ED
        mov     edx,dword ptr [esi+HC_CTLDUMMY]
        cmp     edi,dword ptr [esi+HC_CTLQ]
        je      ohr_dummy
        mov     edx,dword ptr [esi+HC_BLKDUMMY]
ohr_dummy:
        ; the chain's last descriptor leads to the placeholder
        mov     eax,ebp
ohr_last:
        cmp     dword ptr [eax+EL_NEXT],0
        je      ohr_islast
        mov     eax,dword ptr [eax+EL_NEXT]
        jmp     ohr_last
ohr_islast:
        mov     dword ptr [eax+EL_HW2],edx
        mov     dword ptr [edx+EL_HW0],OHT_FRESH         ; a clean placeholder
        mov     dword ptr [edx+EL_HW1],0
        mov     dword ptr [edx+EL_HW2],0
        mov     dword ptr [edx+EL_HW3],0
        ; the ED: address, endpoint, direction, speed, size
        movzx   eax,byte ptr [ebx+USBD_ADDR]
        push    ecx
        and     ecx,0Fh
        shl     ecx,OHE_EN_SHIFT
        or      eax,ecx
        pop     ecx
        test    cl,80h
        jz      ohr_notin
        or      eax,OHE_D_IN
        jmp     ohr_dir
ohr_notin:
        test    cl,0Fh
        jz      ohr_dir                 ; endpoint 0: the direction is the
        or      eax,OHE_D_OUT           ; descriptor's own
ohr_dir:
        test    byte ptr [ebx+USBD_FLAGS],USBDF_LOWSPEED
        jz      ohr_fs
        or      eax,OHE_S
ohr_fs:
        push    ecx
        movzx   ecx,dx
        shl     ecx,OHE_MPS_SHIFT
        or      eax,ecx
        pop     ecx
        or      eax,OHE_K               ; still skipped while the rest goes in
        mov     dword ptr [edi+EL_HW0],eax
        mov     dword ptr [edi+EL_HW1],edx      ; TailP: the placeholder
        mov     eax,ebp                         ; HeadP: the chain, with
        test    ch,1                            ; the carry
        jz      ohr_carry0
        or      eax,OHE_CARRY
ohr_carry0:
        mov     dword ptr [edi+EL_HW2],eax
        and     dword ptr [edi+EL_HW0],NOT OHE_K
        ; and tell the controller the list has something in it
        mov     edx,dword ptr [esi+HC_BASE]
        mov     eax,OHS_CLF
        cmp     edi,dword ptr [esi+HC_CTLQ]
        je      ohr_filled
        mov     eax,OHS_BLF
ohr_filled:
        mov     dword ptr [edx+OHR_CMDSTATUS],eax
        popad
        ret

;---------------------------------------------------------------------
; oh_stop - EDI = the ED: skipped, emptied, and the halt cleared.  The
;           controller may be inside it when this is called - a chain
;           that timed out is still being retried - so the skip is
;           given a frame to take effect before the pointers move.
;---------------------------------------------------------------------
oh_stop:
        push    eax
        push    ecx
        push    edx
        or      dword ptr [edi+EL_HW0],OHE_K
        mov     ecx,1500
        call    usb_udelay
        mov     edx,dword ptr [esi+HC_CTLDUMMY]
        cmp     edi,dword ptr [esi+HC_CTLQ]
        je      ohs_dummy
        mov     edx,dword ptr [esi+HC_BLKDUMMY]
ohs_dummy:
        mov     eax,dword ptr [edi+EL_HW2]
        and     eax,OHE_CARRY           ; the carry survives, the halt not
        or      eax,edx
        mov     dword ptr [edi+EL_HW2],eax
        mov     dword ptr [edi+EL_HW1],edx
        pop     edx
        pop     ecx
        pop     eax
        ret

;--- oh_skipto - a data stage here is one descriptor, so a short packet
;--- is followed by the status stage without help
oh_skipto:
        ret

;---------------------------------------------------------------------
; oh_tdstat - EAX = a descriptor -> AL = XS_*, AH = USBE_*, ECX = bytes
;             moved.  A descriptor the controller has finished with has
;             CBP zero when every byte went, or pointing at the first
;             byte that did not.
;---------------------------------------------------------------------
oh_tdstat:
        push    edx
        mov     edx,dword ptr [eax+EL_HW0]
        shr     edx,OHT_CC_SHIFT
        cmp     dl,OHCC_FRESH
        je      ots_pending
        cmp     dl,OHCC_FRESH2
        je      ots_pending
        or      dl,dl
        jnz     ots_error
        movzx   ecx,word ptr [eax+EL_LEN16]
        mov     edx,dword ptr [eax+EL_HW1]      ; CBP
        or      edx,edx
        jz      ots_all
        sub     edx,dword ptr [eax+EL_BUF]
        mov     ecx,edx
ots_all:
        mov     al,XS_DONE
        xor     ah,ah
        pop     edx
        ret
ots_pending:
        mov     al,XS_PENDING
        xor     ah,ah
        xor     ecx,ecx
        pop     edx
        ret
ots_error:
        mov     ah,USBE_STALL
        cmp     dl,OHCC_STALL
        je      ots_err
        mov     ah,USBE_TIMEOUT
        cmp     dl,OHCC_NOANSWER
        je      ots_err
        mov     ah,USBE_BABBLE
        cmp     dl,OHCC_OVERRUN
        je      ots_err
        mov     ah,USBE_CRC
        cmp     dl,OHCC_CRC
        je      ots_err
        cmp     dl,OHCC_BITSTUFF
        je      ots_err
        mov     ah,USBE_OTHER
ots_err:
        mov     al,XS_ERROR
        xor     ecx,ecx
        pop     edx
        ret

;--- oh_qtoggle - EDI = an ED -> AL = its toggle carry
oh_qtoggle:
        mov     al,byte ptr [edi+EL_HW2]
        shr     al,1
        and     al,1
        ret

;=====================================================================
; Interrupt EDs and their rings.
;=====================================================================

;---------------------------------------------------------------------
; oh_intrq - EBX -> device -> EDI = a new ED for its interrupt IN
;            endpoint, at the head of every interrupt table entry so
;            that it is served every frame.  Empty: oh_ringadd fills
;            it.  CF if the pool is empty.
;---------------------------------------------------------------------
oh_intrq:
        push    eax
        push    ecx
        push    edx
        call    el_alloc
        jc      oiq_out
        mov     edi,eax
        movzx   eax,byte ptr [ebx+USBD_ADDR]
        movzx   ecx,byte ptr [ebx+USBD_EPIN]
        shl     ecx,OHE_EN_SHIFT
        or      eax,ecx
        or      eax,OHE_D_IN
        test    byte ptr [ebx+USBD_FLAGS],USBDF_LOWSPEED
        jz      oiq_fs
        or      eax,OHE_S
oiq_fs:
        movzx   ecx,word ptr [ebx+USBD_MAXPIN]
        cmp     ecx,RING_BUF
        jbe     oiq_mps
        mov     ecx,RING_BUF            ; the ring's buffers are this big
oiq_mps:
        shl     ecx,OHE_MPS_SHIFT
        or      eax,ecx
        or      eax,OHE_K               ; skipped until it has a queue
        mov     dword ptr [edi+EL_HW0],eax
        mov     dword ptr [edi+EL_HW1],0
        mov     dword ptr [edi+EL_HW2],0
        mov     edx,dword ptr [esi+HC_INTHEAD]
        mov     dword ptr [edi+EL_HW3],edx      ; NextED: the old head
        mov     dword ptr [esi+HC_INTHEAD],edi
        ; every entry of the interrupt table
        push    edi
        mov     eax,edi
        mov     edi,dword ptr [esi+HC_FL]
        mov     ecx,32
        rep     stosd
        pop     edi
        clc
oiq_out:
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; oh_intrdel - EDI = a HID ED: skipped and out of the list.  Freed by
;              the caller a tick later, when the controller cannot be
;              in it.
;---------------------------------------------------------------------
oh_intrdel:
        push    eax
        push    ecx
        push    edx
        or      dword ptr [edi+EL_HW0],OHE_K
        mov     eax,dword ptr [edi+EL_HW3]      ; the one after it
        cmp     edi,dword ptr [esi+HC_INTHEAD]
        jne     oid_middle
        mov     dword ptr [esi+HC_INTHEAD],eax
        push    edi
        mov     edi,dword ptr [esi+HC_FL]
        mov     ecx,32
        rep     stosd
        pop     edi
        jmp     oid_out
oid_middle:
        mov     edx,dword ptr [esi+HC_INTHEAD]
oid_walk:
        or      edx,edx
        jz      oid_out
        cmp     dword ptr [edx+EL_HW3],edi
        je      oid_prev
        mov     edx,dword ptr [edx+EL_HW3]
        jmp     oid_walk
oid_prev:
        mov     dword ptr [edx+EL_HW3],eax
oid_out:
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; oh_ringadd - EDI = the ED, EAX = a descriptor with EL_BUF and
;              EL_LEN16 set, EBX -> its device.  The first one added
;              becomes the placeholder; each one after fills the
;              placeholder as a real IN and becomes the placeholder
;              itself - so N added is N-1 queued and one waiting.
;---------------------------------------------------------------------
oh_ringadd:
        push    ecx
        push    edx
        or      byte ptr [eax+EL_FLAGS],ELF_IN OR ELF_BULK OR ELF_DUMMY
        mov     dword ptr [eax+EL_HW0],OHT_FRESH
        mov     dword ptr [eax+EL_HW1],0
        mov     dword ptr [eax+EL_HW2],0
        mov     dword ptr [eax+EL_HW3],0
        mov     edx,dword ptr [ebx+USBD_RING]
        or      edx,edx
        jnz     ora_more
        ; the first: the placeholder of an empty queue
        mov     dword ptr [eax+EL_NEXT],eax
        mov     dword ptr [eax+EL_PREV],eax
        mov     dword ptr [ebx+USBD_RING],eax
        mov     dword ptr [edi+EL_HW1],eax      ; TailP
        mov     dword ptr [edi+EL_HW2],eax      ; HeadP
        and     dword ptr [edi+EL_HW0],NOT OHE_K
        jmp     ora_out
ora_more:
        ; into the circle behind the last...
        push    ebx
        mov     ebx,dword ptr [edx+EL_PREV]     ; EBX = the last = placeholder
        mov     dword ptr [ebx+EL_NEXT],eax
        mov     dword ptr [eax+EL_PREV],ebx
        mov     dword ptr [eax+EL_NEXT],edx
        mov     dword ptr [edx+EL_PREV],eax
        ; ...and the old placeholder becomes a real IN leading to it
        mov     ecx,dword ptr [ebx+EL_BUF]
        mov     dword ptr [ebx+EL_HW1],ecx
        movzx   edx,word ptr [ebx+EL_LEN16]
        add     ecx,edx
        dec     ecx
        mov     dword ptr [ebx+EL_HW3],ecx
        mov     dword ptr [ebx+EL_HW2],eax
        mov     dword ptr [ebx+EL_HW0],OHT_FRESH OR OHT_DP_IN OR OHT_R
        and     byte ptr [ebx+EL_FLAGS],NOT ELF_DUMMY
        pop     ebx
        mov     dword ptr [edi+EL_HW1],eax      ; TailP: the new placeholder
ora_out:
        pop     edx
        pop     ecx
        ret

;--- oh_ringst - EAX = a ring descriptor -> AL = XS_*, ECX = bytes.
;--- The placeholder is always pending.
oh_ringst:
        test    byte ptr [eax+EL_FLAGS],ELF_DUMMY
        jnz     ors_pending
        call    oh_tdstat
        ret
ors_pending:
        mov     al,XS_PENDING
        xor     ah,ah
        xor     ecx,ecx
        ret

;---------------------------------------------------------------------
; oh_rearm - EAX = the ring descriptor just read: it becomes the
;            placeholder, and the old placeholder - the one before it
;            round the circle - becomes a real IN behind the rest.
;            EDI = the ED, which is unhalted if an error halted it.
;---------------------------------------------------------------------
oh_rearm:
        push    ebx
        push    ecx
        push    edx
        mov     ebx,dword ptr [eax+EL_PREV]     ; the old placeholder
        ; the read one, clean, as the placeholder
        mov     dword ptr [eax+EL_HW0],OHT_FRESH
        mov     dword ptr [eax+EL_HW1],0
        mov     dword ptr [eax+EL_HW2],0
        mov     dword ptr [eax+EL_HW3],0
        or      byte ptr [eax+EL_FLAGS],ELF_DUMMY
        ; the old one, real, leading to it
        mov     ecx,dword ptr [ebx+EL_BUF]
        mov     dword ptr [ebx+EL_HW1],ecx
        movzx   edx,word ptr [ebx+EL_LEN16]
        add     ecx,edx
        dec     ecx
        mov     dword ptr [ebx+EL_HW3],ecx
        mov     dword ptr [ebx+EL_HW2],eax
        mov     dword ptr [ebx+EL_HW0],OHT_FRESH OR OHT_DP_IN OR OHT_R
        and     byte ptr [ebx+EL_FLAGS],NOT ELF_DUMMY
        mov     dword ptr [edi+EL_HW1],eax      ; TailP
        ; an error halts the ED, and HeadP then carries the halt bit
        test    dword ptr [edi+EL_HW2],OHE_HALT
        jz      orm_out
        and     dword ptr [edi+EL_HW2],NOT OHE_HALT
orm_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- oh_ack - the done queue, taken off the controller's hands
oh_ack:
        push    eax
        push    edi
        mov     edi,dword ptr [esi+HC_BASE]
        test    dword ptr [edi+OHR_INTSTATUS],OHI_WDH
        jz      oak_out
        mov     eax,dword ptr [esi+HC_FL]
        mov     dword ptr [eax+HCCA_DONEHEAD],0
        mov     dword ptr [edi+OHR_INTSTATUS],OHI_WDH
oak_out:
        pop     edi
        pop     eax
        ret

;=====================================================================
; State
;=====================================================================
oh_tpid     db  0                       ; oh_mktd's inputs, parked
oh_ttog     db  0
oh_tflags   db  0
            db  0
oh_tbuf     dd  0
oh_tlen     dd  0
