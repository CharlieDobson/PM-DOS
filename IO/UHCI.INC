;=====================================================================
; UHCI.INC - Intel's Universal Host Controller: the PIIX3, PIIX4 and
;            ICH south bridges, and VIA's, in I/O space
;
; The controller walks a FRAME LIST of 1024 pointers, one a
; millisecond, and each pointer leads to a chain of queue heads and
; transfer descriptors: a descriptor is ONE PACKET - eight bytes to a
; low-speed keyboard, sixty-four to a disk - with its own address,
; endpoint, toggle and buffer, and the controller writes its status
; back into it.  Every entry of the frame list points at the same
; skeleton here:
;
;   skeleton QH  --element-->  HID QH  ->  HID QH  ->  control QH  ->
;                 ...                                   bulk QH  ->  end
;
; A HID queue head holds a RING of descriptors, each an IN of one
; report, linked round in a circle and marked breadth-first: the
; controller runs one of them a frame and goes on to the next queue,
; so a keyboard is asked once a millisecond and the answers pile up
; in the ring for the tick to collect.  The control and bulk queues
; hold one chain at a time, depth-first, so that a bulk transfer takes
; as many packets a frame as the bus has room for.
;
; The BIOS's legacy emulation is the LEGSUP word in configuration
; space, which routes port 60h and 64h to SMI; it is written off, and
; written back at a park.  This driver takes no interrupt: USBINTR is
; zero and the status is read out of the descriptors.
;
; Intel's UHCI 1.1 design guide is the reference, and Linux's uhci-hcd
; for what the chips do that the guide does not say.  Tested on QEMU's
; piix3-usb-uhci, piix4-usb-uhci and ich9-usb-uhci.
;
; Included by IOPM.ASM after USB.INC.
;=====================================================================

;--- the registers, from the I/O base --------------------------------
UHR_CMD         EQU     00h             ; dw
UHR_STS         EQU     02h             ; dw, write 1 to clear
UHR_INTR        EQU     04h             ; dw
UHR_FRNUM       EQU     06h             ; dw
UHR_FLBASE      EQU     08h             ; dd
UHR_SOF         EQU     0Ch             ; db
UHR_PORTSC      EQU     10h             ; dw, one a port

UHC_RS          EQU     0001h           ; run
UHC_HCRESET     EQU     0002h
UHC_GRESET      EQU     0004h           ; global reset: the bus too
UHC_CF          EQU     0040h           ; "configure flag": a driver is here
UHC_MAXP        EQU     0080h           ; 64-byte packets allowed

UHS_HALTED      EQU     0020h
UHS_ALL         EQU     003Fh

UHP_CCS         EQU     0001h           ; connected
UHP_CSC         EQU     0002h           ; ...changed (write 1 to clear)
UHP_PED         EQU     0004h           ; enabled
UHP_PEDC        EQU     0008h           ; ...changed (write 1 to clear)
UHP_LSDA        EQU     0100h           ; low-speed device
UHP_PR          EQU     0200h           ; port reset
UHP_SUSP        EQU     1000h
UHP_RWC         EQU     UHP_CSC OR UHP_PEDC

UHPCI_LEGSUP    EQU     0C0h            ; dw in configuration space
UHLEG_RWC       EQU     8F00h           ; its write-one-to-clear bits

;--- a descriptor ----------------------------------------------------
; dword 0: the link, as a frame list entry: bit 0 T (nothing follows),
;          bit 1 Q (what follows is a QH), bit 2 Vf (depth first)
UHL_T           EQU     1
UHL_Q           EQU     2
UHL_VF          EQU     4
; dword 1: control and status
UHT_ACTLEN      EQU     000007FFh       ; bytes moved, less one
UHT_BITSTUFF    EQU     00020000h
UHT_CRCTO       EQU     00040000h       ; CRC error, or no answer
UHT_NAK         EQU     00080000h
UHT_BABBLE      EQU     00100000h
UHT_DBUF        EQU     00200000h       ; data buffer error
UHT_STALLED     EQU     00400000h
UHT_ACTIVE      EQU     00800000h
UHT_IOC         EQU     01000000h
UHT_IOS         EQU     02000000h
UHT_LS          EQU     04000000h       ; a low-speed device
UHT_CERR3       EQU     18000000h       ; three tries at an error
UHT_SPD         EQU     20000000h       ; a short packet stops the queue
UHT_ERRORS      EQU     UHT_BITSTUFF OR UHT_CRCTO OR UHT_BABBLE OR UHT_DBUF OR UHT_STALLED
; dword 2: the token - PID, address, endpoint, toggle, length less one
;          in bits 21-31
; dword 3: the buffer

UH_HALTWAIT     EQU     200             ; 10ms, in 50us steps

;=====================================================================
; uh_init - ESI -> the controller's row: taken from the BIOS, reset,
;           and running an empty schedule.  CF if its registers could
;           not be found.
;=====================================================================
uh_init:
        pushad
        mov     ebx,dword ptr [esi+HC_PCI]
        mov     dl,4                    ; BAR4: the I/O registers
        call    pcx_bar                 ; -> EAX base, EDX flags
        jc      uhi_fail
        test    edx,BARF_IO
        jz      uhi_fail
        test    edx,BARF_NONE
        jnz     uhi_fail
        or      eax,eax
        jz      uhi_fail
        movzx   eax,ax
        mov     dword ptr [esi+HC_BASE],eax
        ; I/O decoding and bus mastering on, whatever the BIOS left
        mov     al,PCIR_COMMAND
        call    pci_read
        mov     ecx,eax
        and     ecx,0000FFFFh
        or      ecx,PCIC_IOEN OR PCIC_BUSMASTER
        mov     al,PCIR_COMMAND
        call    pci_write

        ; what the BIOS had, for the hand-back
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_CMD
        in      ax,dx
        mov     word ptr [esi+HC_SAVE+0],ax
        add     dx,UHR_INTR - UHR_CMD
        in      ax,dx
        mov     word ptr [esi+HC_SAVE+2],ax
        add     dx,UHR_FRNUM - UHR_INTR
        in      ax,dx
        mov     word ptr [esi+HC_SAVE+4],ax
        add     dx,UHR_FLBASE - UHR_FRNUM
        in      eax,dx
        mov     dword ptr [esi+HC_SAVE+6],eax
        add     dx,UHR_SOF - UHR_FLBASE
        in      al,dx
        mov     byte ptr [esi+HC_SAVE+10],al
        mov     dl,UHPCI_LEGSUP
        mov     dh,2
        call    pcx_rd
        mov     word ptr [esi+HC_SAVE+12],ax
        test    ax,001Fh                ; any trap or SMI enable on: the
        jz      uhi_nolegacy            ; BIOS was emulating through it
        or      byte ptr [esi+HC_FLAGS],USBHF_LEGACY
uhi_nolegacy:
        ; the legacy register: every status bit written off, every
        ; enable off - no SMI on a keyboard port, no PIRQ either, since
        ; nothing here takes an interrupt
        mov     ecx,UHLEG_RWC
        call    pcx_wr
        xor     ecx,ecx
        call    pcx_wr

        ; a global reset puts the bus and every device on it back to
        ; the start; 50ms is what the specification asks for
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_CMD
        mov     ax,UHC_GRESET
        out     dx,ax
        mov     ecx,1
        call    usb_waitticks
        xor     ax,ax
        out     dx,ax
        mov     ecx,100
        call    usb_udelay
        ; then the controller's own reset, which the chip ends itself
        mov     ax,UHC_HCRESET
        out     dx,ax
        mov     ecx,100
uhi_rst:
        in      ax,dx
        test    ax,UHC_HCRESET
        jz      uhi_reset
        push    ecx
        mov     ecx,10
        call    usb_udelay
        pop     ecx
        loop    uhi_rst
        jmp     uhi_fail
uhi_reset:
        add     dx,UHR_INTR - UHR_CMD
        xor     ax,ax
        out     dx,ax                   ; no interrupts
        add     dx,UHR_STS - UHR_INTR
        mov     ax,UHS_ALL
        out     dx,ax                   ; nothing outstanding

        ; the skeleton: three queue heads and a frame list that points
        ; at the first of them
        call    el_alloc
        jc      uhi_fail
        mov     dword ptr [esi+HC_INTQ],eax
        call    el_alloc
        jc      uhi_fail
        mov     dword ptr [esi+HC_CTLQ],eax
        call    el_alloc
        jc      uhi_fail
        mov     dword ptr [esi+HC_BLKQ],eax
        mov     dword ptr [eax+EL_HW0],UHL_T    ; bulk: last of the row
        mov     dword ptr [eax+EL_HW1],UHL_T    ; ...and empty
        mov     edi,dword ptr [esi+HC_CTLQ]
        or      eax,UHL_Q
        mov     dword ptr [edi+EL_HW0],eax      ; control -> bulk
        mov     dword ptr [edi+EL_HW1],UHL_T
        mov     eax,edi
        or      eax,UHL_Q
        mov     edi,dword ptr [esi+HC_INTQ]
        mov     dword ptr [edi+EL_HW0],eax      ; skeleton -> control
        mov     dword ptr [edi+EL_HW1],UHL_T    ; no HID queues yet
        mov     dword ptr [esi+HC_INTHEAD],0
        mov     eax,edi
        or      eax,UHL_Q
        mov     edi,dword ptr [esi+HC_FL]
        mov     ecx,1024
        rep     stosd

        ; and go: the list, frame 0, a 1ms frame, run
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_FLBASE
        mov     eax,dword ptr [esi+HC_FL]
        out     dx,eax
        add     dx,UHR_FRNUM - UHR_FLBASE
        xor     ax,ax
        out     dx,ax
        add     dx,UHR_SOF - UHR_FRNUM
        mov     al,64
        out     dx,al
        add     dx,UHR_CMD - UHR_SOF
        mov     ax,UHC_RS OR UHC_CF OR UHC_MAXP
        out     dx,ax

        ; the ports: a register that reads with bit 7 set and is not
        ; all ones is one.  Two, on everything but a few that have more
        mov     byte ptr [esi+HC_NPORTS],0
        add     dx,UHR_PORTSC - UHR_CMD
        mov     ecx,7
uhi_ports:
        in      ax,dx
        cmp     ax,0FFFFh
        je      uhi_portsdone
        test    al,80h
        jz      uhi_portsdone
        inc     byte ptr [esi+HC_NPORTS]
        add     dx,2
        loop    uhi_ports
uhi_portsdone:
        cmp     byte ptr [esi+HC_NPORTS],2
        jae     uhi_ok
        mov     byte ptr [esi+HC_NPORTS],2
uhi_ok:
IFDEF HANG_E9
        mov     al,'I'
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
        add     dx,2
        in      eax,dx
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [esi+HC_FL]
        mov     eax,dword ptr [eax]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [esi+HC_INTQ]
        mov     eax,dword ptr [eax+EL_HW0]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [esi+HC_INTQ]
        mov     eax,dword ptr [eax+EL_HW1]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr [esi+HC_CTLQ]
        mov     eax,dword ptr [eax+EL_HW0]
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
ENDIF
        popad
        clc
        ret
uhi_fail:
        popad
        stc
        ret

;=====================================================================
; uh_handback - stopped, and the registers as the BIOS left them.
;=====================================================================
uh_handback:
        pushad
        call    uh_stoprun
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_FLBASE
        mov     eax,dword ptr [esi+HC_SAVE+6]
        out     dx,eax
        add     dx,UHR_FRNUM - UHR_FLBASE
        mov     ax,word ptr [esi+HC_SAVE+4]
        and     ax,07FFh
        out     dx,ax
        add     dx,UHR_SOF - UHR_FRNUM
        mov     al,byte ptr [esi+HC_SAVE+10]
        out     dx,al
        add     dx,UHR_INTR - UHR_SOF
        mov     ax,word ptr [esi+HC_SAVE+2]
        out     dx,ax
        add     dx,UHR_CMD - UHR_INTR
        mov     ax,word ptr [esi+HC_SAVE+0]
        out     dx,ax
        mov     ebx,dword ptr [esi+HC_PCI]
        mov     dl,UHPCI_LEGSUP
        mov     dh,2
        movzx   ecx,word ptr [esi+HC_SAVE+12]
        and     ecx,NOT UHLEG_RWC       ; the status bits are not for writing
        call    pcx_wr
        popad
        ret

;--- uh_stoprun - run off, and wait for the controller to say it has
;--- halted (a frame at most)
uh_stoprun:
        push    eax
        push    ecx
        push    edx
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_CMD
        in      ax,dx
        and     ax,NOT UHC_RS
        out     dx,ax
        add     dx,UHR_STS - UHR_CMD
        mov     ecx,UH_HALTWAIT
ushr_lp:
        in      ax,dx
        test    ax,UHS_HALTED
        jnz     ushr_out
        push    ecx
        mov     ecx,50
        call    usb_udelay
        pop     ecx
        loop    ushr_lp
ushr_out:
        pop     edx
        pop     ecx
        pop     eax
        ret

;--- uh_runsched - Run/Stop on again, the configure flag and 64-byte
;--- packets kept as uh_init set them.  ESI -> the controller's row.  For
;--- the WfW-bridge keepalive a controller PAUSED at the park (its Run bit
;--- cleared by uh_stoprun, the frame list and queues left exactly as
;--- built) is woken for the span of one bridge transfer and paused again,
;--- so it bus-masters nothing while Windows sets up its GUI.  Everything
;--- kept: setting Run resumes the schedule from the frame it stopped on.
;--- (Always built now; the keepalive is a runtime choice, usb_kalive -
;--- CONFIG.SYS USB=ON,KEEPALIVE - not a build-time one.)
uh_runsched:
        push    eax
        push    edx
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_CMD
        mov     ax,UHC_RS OR UHC_CF OR UHC_MAXP
        out     dx,ax
        pop     edx
        pop     eax
        ret

;=====================================================================
; uh_halt - stopped and reset, for a restart of the machine.
;=====================================================================
uh_halt:
        pushad
        call    uh_stoprun
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_CMD
        mov     ax,UHC_HCRESET
        out     dx,ax
        popad
        ret

;=====================================================================
; The ports.  CL = the port, from 1.
;=====================================================================
uh_portdx:                              ; -> DX = its register
        mov     edx,dword ptr [esi+HC_BASE]
        add     dx,UHR_PORTSC - 2
        push    ecx
        movzx   ecx,cl
        shl     ecx,1
        add     dx,cx
        pop     ecx
        ret

uh_portst:
        push    edx
        call    uh_portdx
        in      ax,dx
        mov     dx,ax
        xor     al,al
        test    dl,UHP_CCS
        jz      ups_nocon
        or      al,PST_CONNECT
ups_nocon:
        test    dl,UHP_CSC
        jz      ups_nochg
        or      al,PST_CHANGE
ups_nochg:
        test    dl,UHP_PED
        jz      ups_nope
        or      al,PST_ENABLED
ups_nope:
        test    dh,UHP_LSDA SHR 8
        jz      ups_fs
        or      al,PST_LOWSPEED
ups_fs:
        pop     edx
        ret

uh_portack:
        push    eax
        push    edx
        call    uh_portdx
        in      ax,dx
        and     ax,NOT UHP_PR           ; never a reset by accident
        or      ax,UHP_RWC              ; the change bits, written off
        out     dx,ax
        pop     edx
        pop     eax
        ret

uh_portreset:
        push    eax
        push    edx
        call    uh_portdx
        in      ax,dx
        and     ax,NOT UHP_RWC
        or      ax,UHP_PR
        out     dx,ax
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; uh_portenable - the reset released and the port enabled.  The guide
;                 wants the enable within a few milliseconds of the
;                 release, and some chips take the write twice.
;---------------------------------------------------------------------
uh_portenable:
        push    eax
        push    ecx
        push    edx
        call    uh_portdx
        in      ax,dx
        and     ax,NOT (UHP_PR OR UHP_RWC)
        out     dx,ax
        mov     ecx,100                 ; until PR reads clear, a ms at most
upe_wait:
        in      ax,dx
        test    ax,UHP_PR
        jz      upe_released
        push    ecx
        mov     ecx,10
        call    usb_udelay
        pop     ecx
        loop    upe_wait
upe_released:
        mov     ecx,100
        call    usb_udelay
        mov     ecx,3
upe_enable:
        in      ax,dx
        and     ax,NOT UHP_PR
        or      ax,UHP_PED OR UHP_RWC
        out     dx,ax
        push    ecx
        mov     ecx,100
        call    usb_udelay
        pop     ecx
        in      ax,dx
        test    ax,UHP_PED
        jnz     upe_out
        loop    upe_enable
upe_out:
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; Descriptors.
;=====================================================================

;---------------------------------------------------------------------
; uh_mktd - EBX -> device, DL = PID, DH = endpoint, EDI = buffer, ECX =
;           bytes (one packet's worth at most), AL = toggle, AH = ELF_*
;           -> EAX = the element.  CF if the pool is empty.
;
; The inputs go into statics first so the token can be assembled in
; peace: every register is spoken for on the way in.
;---------------------------------------------------------------------
uh_mktd:
        pushad
        mov     byte ptr [uh_tpid],dl
        mov     byte ptr [uh_tep],dh
        mov     byte ptr [uh_ttog],al
        mov     byte ptr [uh_tflags],ah
        mov     dword ptr [uh_tbuf],edi
        mov     dword ptr [uh_tlen],ecx
        call    el_alloc
        jc      umt2_fail
        mov     ebp,eax
        mov     dword ptr [ebp+EL_HW0],UHL_T
        mov     eax,dword ptr [uh_tbuf]
        mov     dword ptr [ebp+EL_BUF],eax
        mov     dword ptr [ebp+EL_HW3],eax
        mov     eax,dword ptr [uh_tlen]
        mov     word ptr [ebp+EL_LEN16],ax
        mov     al,byte ptr [uh_tflags]
        test    byte ptr [ebx+USBD_FLAGS],USBDF_LOWSPEED
        jz      umt2_fs
        or      al,ELF_LS
umt2_fs:
        mov     [ebp+EL_FLAGS],al
        mov     al,byte ptr [uh_ttog]
        mov     [ebp+EL_TOG],al
        ; the token
        mov     ecx,dword ptr [uh_tlen]
        dec     ecx
        and     ecx,7FFh
        shl     ecx,21
        movzx   edx,byte ptr [uh_tep]
        and     edx,0Fh
        shl     edx,15
        or      ecx,edx
        movzx   edx,byte ptr [ebx+USBD_ADDR]
        shl     edx,8
        or      ecx,edx
        movzx   edx,byte ptr [uh_tpid]
        or      ecx,edx
        test    byte ptr [uh_ttog],1
        jz      umt2_tog0
        or      ecx,00080000h           ; D: DATA1
umt2_tog0:
        mov     dword ptr [ebp+EL_HW2],ecx
        ; control and status: active, three tries, low speed if so, and
        ; a short packet stops the queue on an IN
        mov     eax,ebp
        call    uh_armword              ; -> ECX
        mov     dword ptr [ebp+EL_HW1],ecx
        mov     [esp+28],ebp            ; EAX, through the pushad frame
        popad
        clc
        ret
umt2_fail:
        popad
        stc
        ret

;--- uh_armword - EAX = an element -> ECX = the status dword that
;--- makes it a fresh, active descriptor of its kind
uh_armword:
        mov     ecx,UHT_ACTIVE OR UHT_CERR3
        test    byte ptr [eax+EL_FLAGS],ELF_LS
        jz      uaw_fs
        or      ecx,UHT_LS
uaw_fs:
        ; a control IN stops its queue at a short packet, so that the
        ; status stage can be pointed at (usb_chainpoll).  A bulk IN and
        ; a ring's IN do not: a report shorter than the buffer is the
        ; ordinary case, and the ring has to turn
        test    byte ptr [eax+EL_FLAGS],ELF_IN
        jz      uaw_out
        test    byte ptr [eax+EL_FLAGS],ELF_BULK
        jnz     uaw_out
        or      ecx,UHT_SPD
uaw_out:
        ret

;--- uh_tdmax - a descriptor is a packet: ECX = min(ECX, DX)
uh_tdmax:
        push    eax
        movzx   eax,dx
        cmp     ecx,eax
        jbe     utm_out
        mov     ecx,eax
utm_out:
        pop     eax
        ret

;--- uh_link - EAX's link is EDX, depth first
uh_link:
        push    edx
        or      edx,UHL_VF
        mov     dword ptr [eax+EL_HW0],edx
        pop     edx
        ret

;--- uh_term - EAX ends its chain
uh_term:
        mov     dword ptr [eax+EL_HW0],UHL_T
        ret

;--- uh_run - the queue EDI's element pointer is EAX
uh_run:
        mov     dword ptr [edi+EL_HW1],eax
        ret

;--- uh_stop - and is nothing
uh_stop:
        mov     dword ptr [edi+EL_HW1],UHL_T
        ret

;--- uh_skipto - and is EAX, further down the chain
uh_skipto:
        mov     dword ptr [edi+EL_HW1],eax
        ret

;---------------------------------------------------------------------
; uh_tdstat - EAX = a descriptor -> AL = XS_*, AH = USBE_*, ECX = bytes
;             moved.
;---------------------------------------------------------------------
uh_tdstat:
        push    edx
        mov     edx,dword ptr [eax+EL_HW1]
        test    edx,UHT_ACTIVE
        jnz     uts_pending
        test    edx,UHT_ERRORS
        jnz     uts_error
        mov     ecx,edx
        and     ecx,UHT_ACTLEN
        inc     ecx
        and     ecx,UHT_ACTLEN          ; 7FFh + 1 is none
        mov     al,XS_DONE
        xor     ah,ah
        pop     edx
        ret
uts_pending:
        mov     al,XS_PENDING
        xor     ah,ah
        xor     ecx,ecx
        pop     edx
        ret
uts_error:
        mov     ah,USBE_STALL
        test    edx,UHT_STALLED
        jnz     uts_err
        mov     ah,USBE_BABBLE
        test    edx,UHT_BABBLE
        jnz     uts_err
        mov     ah,USBE_TIMEOUT
        test    edx,UHT_CRCTO
        jnz     uts_err
        mov     ah,USBE_CRC
uts_err:
        mov     al,XS_ERROR
        xor     ecx,ecx
        pop     edx
        ret

;--- uh_qtoggle - not kept in the queue head on this controller
uh_qtoggle:
        xor     al,al
        ret

;=====================================================================
; Interrupt queues and their rings.
;=====================================================================

;---------------------------------------------------------------------
; uh_intrq - EBX -> device -> EDI = a new queue head at the front of
;            the HID queues.  CF if the pool is empty.
;---------------------------------------------------------------------
uh_intrq:
        push    eax
        push    edx
        call    el_alloc
        jc      uiq_out
        mov     edi,eax
        mov     dword ptr [edi+EL_HW1],UHL_T    ; no descriptors yet
        ; horizontal: whatever was first, else the control queue
        mov     edx,dword ptr [esi+HC_INTHEAD]
        or      edx,edx
        jnz     uiq_next
        mov     edx,dword ptr [esi+HC_CTLQ]
uiq_next:
        or      edx,UHL_Q
        mov     dword ptr [edi+EL_HW0],edx
        mov     dword ptr [esi+HC_INTHEAD],edi
        mov     eax,edi
        or      eax,UHL_Q
        mov     edx,dword ptr [esi+HC_INTQ]
        mov     dword ptr [edx+EL_HW1],eax      ; the skeleton leads to it
        clc
uiq_out:
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; uh_intrdel - EDI = a HID queue head: out of the row.  The element is
;              freed by the caller a tick later.
;---------------------------------------------------------------------
uh_intrdel:
        push    eax
        push    edx
        mov     eax,dword ptr [edi+EL_HW0]      ; what followed it
        cmp     edi,dword ptr [esi+HC_INTHEAD]
        jne     uid_middle
        ; the first: the skeleton's element becomes the next, or
        ; nothing if the next is the control queue
        mov     edx,eax
        and     edx,NOT 0Fh
        cmp     edx,dword ptr [esi+HC_CTLQ]
        jne     uid_head
        mov     dword ptr [esi+HC_INTHEAD],0
        mov     eax,UHL_T
        jmp     uid_write
uid_head:
        mov     dword ptr [esi+HC_INTHEAD],edx
uid_write:
        mov     edx,dword ptr [esi+HC_INTQ]
        mov     dword ptr [edx+EL_HW1],eax
        jmp     uid_out
uid_middle:
        ; find the one before it
        mov     edx,dword ptr [esi+HC_INTHEAD]
uid_walk:
        or      edx,edx
        jz      uid_out
        push    eax
        mov     eax,dword ptr [edx+EL_HW0]
        and     eax,NOT 0Fh
        cmp     eax,edi
        pop     eax
        je      uid_prev
        mov     edx,dword ptr [edx+EL_HW0]
        and     edx,NOT 0Fh
        cmp     edx,dword ptr [esi+HC_CTLQ]
        jne     uid_walk
        jmp     uid_out
uid_prev:
        mov     dword ptr [edx+EL_HW0],eax
uid_out:
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; uh_ringadd - EDI = queue, EAX = a descriptor with EL_BUF, EL_LEN16,
;              EL_TOG and EL_FLAGS set, EBX -> its device: built as an
;              IN of one packet and queued behind the ring's others,
;              the circle closed through it.  Breadth-first: the ring
;              turns one descriptor a frame.
;---------------------------------------------------------------------
uh_ringadd:
        push    ecx
        push    edx
        ; the token
        movzx   ecx,word ptr [eax+EL_LEN16]
        dec     ecx
        and     ecx,7FFh
        shl     ecx,21
        movzx   edx,byte ptr [ebx+USBD_EPIN]
        shl     edx,15
        or      ecx,edx
        movzx   edx,byte ptr [ebx+USBD_ADDR]
        shl     edx,8
        or      ecx,edx
        or      ecx,PID_IN
        test    byte ptr [eax+EL_TOG],1
        jz      ura_tog0
        or      ecx,00080000h
ura_tog0:
        mov     dword ptr [eax+EL_HW2],ecx
        mov     edx,dword ptr [eax+EL_BUF]
        mov     dword ptr [eax+EL_HW3],edx
        or      byte ptr [eax+EL_FLAGS],ELF_IN OR ELF_BULK
        test    byte ptr [ebx+USBD_FLAGS],USBDF_LOWSPEED
        jz      ura_fs
        or      byte ptr [eax+EL_FLAGS],ELF_LS
ura_fs:
        call    uh_armword
        mov     dword ptr [eax+EL_HW1],ecx
        ; into the circle
        mov     edx,dword ptr [ebx+USBD_RING]
        or      edx,edx
        jnz     ura_more
        ; the first: a circle of one, and the queue points at it
        mov     dword ptr [eax+EL_HW0],eax      ; breadth first: no Vf
        mov     dword ptr [eax+EL_NEXT],eax
        mov     dword ptr [eax+EL_PREV],eax
        mov     dword ptr [ebx+USBD_RING],eax
        mov     dword ptr [edi+EL_HW1],eax
        jmp     ura_out
ura_more:
        ; behind the last, which is the first's predecessor
        push    ebx
        mov     ebx,dword ptr [edx+EL_PREV]     ; EBX = the last
        mov     dword ptr [ebx+EL_HW0],eax
        mov     dword ptr [ebx+EL_NEXT],eax
        mov     dword ptr [eax+EL_PREV],ebx
        mov     dword ptr [eax+EL_NEXT],edx
        mov     dword ptr [eax+EL_HW0],edx
        mov     dword ptr [edx+EL_PREV],eax
        pop     ebx
ura_out:
        pop     edx
        pop     ecx
        ret

;--- uh_ringst - EAX = a ring descriptor -> AL = XS_*, ECX = bytes
uh_ringst:
        call    uh_tdstat
        ret

;--- uh_rearm - EAX = a ring descriptor that has been read: active
;--- again, with its toggle as built
uh_rearm:
        push    ecx
        push    edx
        mov     edx,dword ptr [eax+EL_BUF]
        mov     dword ptr [eax+EL_HW3],edx
        call    uh_armword
        mov     dword ptr [eax+EL_HW1],ecx
        pop     edx
        pop     ecx
        ret

;--- uh_ack - nothing a tick on this controller
uh_ack:
        ret

;=====================================================================
; State
;=====================================================================
uh_tpid     db  0                       ; uh_mktd's inputs, parked
uh_tep      db  0
uh_ttog     db  0
uh_tflags   db  0
uh_tbuf     dd  0
uh_tlen     dd  0
