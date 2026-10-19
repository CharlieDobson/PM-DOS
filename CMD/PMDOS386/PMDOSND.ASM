; PMDOSND.ASM - a minimal NDIS 2.0 real-mode MAC "shim" for the PM-DOS
; Windows-for-Workgroups bridge.
;
; WfW's binding flow needs a real-mode NDIS2 MAC registered with the
; Protocol Manager before enhanced-mode NDIS.386 will bind a protocol
; (NetBEUI) to the adapter.  The bridge card is owned by the parked
; PM-DOS kernel and is reachable only through the ring-0 bridge (which
; does not exist until WfW loads PMDOS.386), so there is no real card to
; drive in real mode.  This shim therefore registers the MAC name
; "PMDOS$" with the Protocol Manager - satisfying the binding - while the
; actual frames flow through the native PMDOS.386 (this shim's frame
; handlers are stubs and are not used for the data path).
;
; It is a COM-style DOS character device driver (header at offset 0).
; The NDIS 2.0 structures and the Protocol Manager IOCTL handshake were
; recovered from the in-box NDISHLP.SYS + NE2000.DOS.
;
; Assembles with Open Watcom wasm and with MASM 6.1x.
;---------------------------------------------------------------------
        .286
_TEXT   SEGMENT BYTE PUBLIC USE16 'CODE'
        ASSUME  cs:_TEXT, ds:_TEXT, es:NOTHING, ss:NOTHING
        ORG     0

;=====================================================================
; RESIDENT - the device header, strategy/interrupt, the NDIS structures
; and the stub handlers all stay resident (the Protocol Manager keeps
; pointers into them).
;=====================================================================

;--- DOS device-driver header ---------------------------------------
dev_header:
        dd      -1                      ; next device: none
        dw      8000h                   ; attributes: character device
        dw      OFFSET strat            ; strategy entry
        dw      OFFSET intr             ; interrupt entry
        db      'PMDOS$  '              ; 8-char device name (\DEV\PMDOS$)

;--- the saved request-packet far pointer (strategy -> interrupt) ----
req_ptr dd      0

;=====================================================================
; Strategy: DOS calls it with ES:BX -> the request packet.  Save it.
;=====================================================================
strat:
        mov     word ptr cs:[req_ptr],bx
        mov     word ptr cs:[req_ptr+2],es
        retf

;=====================================================================
; Interrupt: dispatch on the request packet's command byte.  Only INIT
; (command 0) does real work; every other command is answered "done".
;=====================================================================
intr:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        push    cs
        pop     ds                      ; DS = our segment
        les     bx,[req_ptr]            ; ES:BX -> request packet
        mov     al,es:[bx+2]            ; command code
        or      al,al
        jnz     ic_done                 ; not INIT: just mark done
        call    do_init
        jmp     ic_exit
ic_done:
        mov     word ptr es:[bx+3],0100h ; status = done
ic_exit:
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

;=====================================================================
; The NDIS 2.0 MAC structures.  The far-pointer SEGMENT words are 0 here
; and are patched to our load segment at init (fill_segs).  Values and
; layout mirror the in-box NE2000.DOS (medium DIX+802.3, 10 Mbit,
; 1514-byte frames), with our own name and handlers.
;=====================================================================

;--- MAC Common Characteristics Table (0x40 bytes) ------------------
common_chars:
        dw      0040h                   ; +00 TableSize
        db      2, 0                    ; +02 NDIS major/minor
        db      1, 0                    ; +04 (as NE2000)
        db      0, 2                    ; +06 (as NE2000)
        dd      0                       ; +08 module flags
        db      0                       ; +0C (leading byte, as NE2000)
        db      'PMDOS$', 0             ; +0D module name (ASCIIZ)
        db      8 dup(0)                ; pad name field to +1C
        db      1                       ; +1C upper protocol level
        db      1                       ; +1D upper boundary type
        db      0                       ; +1E lower protocol level
        db      0                       ; +1F lower boundary type
        dw      0                       ; +20 module ID
cc_modds:
        dw      0                       ; +22 module DS (patched)
        dw      OFFSET sysreq           ; +24 SystemRequest offset
cc_sysreq_seg:
        dw      0                       ; +26 (patched)
        dw      OFFSET svc_chars        ; +28 -> service characteristics
cc_svcchar_seg:
        dw      0                       ; +2A (patched)
        dw      OFFSET svc_status       ; +2C -> service status
cc_svcstat_seg:
        dw      0                       ; +2E (patched)
        dw      OFFSET upper_disp        ; +30 -> upper dispatch table
cc_upper_seg:
        dw      0                       ; +32 (patched)
        dd      0                       ; +34 lower dispatch table (none)
        dd      0                       ; +38 reserved
        dd      0                       ; +3C reserved

;--- MAC Service-Specific Characteristics (0x60 bytes) --------------
svc_chars:
        dw      0060h                   ; +00 TableSize
        db      'DIX+802.3', 0          ; +02 medium name
        db      (0Ch - ($ - svc_chars)) dup(0)  ; pad to +0C
        ; station address at +0C (6 bytes) - the kernel card's MAC on the
        ; wire is the native .386's; this placeholder is only read if a
        ; real-mode protocol used this shim's data path (it does not).
mac_addr:
        db      52h, 54h, 00h, 12h, 34h, 56h
        db      (38h - ($ - svc_chars)) dup(0)  ; pad to +0x38
        dd      00989680h               ; +0x38 link speed (10 Mbit, bps)
        dd      0                       ; +0x3C
        dd      000005EAh               ; +0x40 max total frame size (1514)
        db      (5Ch - ($ - svc_chars)) dup(0)  ; pad to +0x5C
        dw      0012h                   ; +0x5C
        dw      0008h                   ; +0x5E

;--- MAC Service Status (0x64 bytes) --------------------------------
svc_status:
        dw      0064h                   ; +00 TableSize
        dd      0FFFFFFFFh              ; +02 (as NE2000)
        db      (0Ch - ($ - svc_status)) dup(0) ; pad to +0x0C
        dw      OFFSET sysreq           ; +0C SystemRequest offset
ss_sysreq_seg:
        dw      0                       ; +0E (patched)
        db      (64h - ($ - svc_status)) dup(0) ; pad to full size

;--- MAC Upper Dispatch Table (far pointers; segs patched) ----------
upper_disp:
        dw      OFFSET upper_disp_back  ; +00 back-pointer offset
ud_back_seg:
        dw      0                       ; +02 (patched)
        dw      OFFSET h_transmit       ; +04 TransmitChain
ud_tx_seg:
        dw      0
        dw      OFFSET h_transfer       ; +08 TransferData
ud_xfer_seg:
        dw      0
        dw      OFFSET h_rcvrelease     ; +0C ReceiveRelease
ud_rr_seg:
        dw      0
        dw      OFFSET h_indicon        ; +10 IndicationOn
ud_ion_seg:
        dw      0
        dw      OFFSET h_indicoff       ; +14 IndicationOff
ud_ioff_seg:
        dw      0
        dw      OFFSET h_reset          ; +18 (6th entry)
ud_rst_seg:
        dw      0
upper_disp_back:
        dd      0                       ; small back-pointer area

;=====================================================================
; Stub upper-edge handlers.  Frames go through the native PMDOS.386, so
; these are not used for the data path; they answer "success" defensively
; in case a real-mode protocol ever calls them.  All are FAR, NDIS cdecl
; (success = AX 0), and clean nothing of the caller's stack here (the
; protocol passes registers / a request block; a minimal success is
; enough for the bind).
;=====================================================================
h_transmit:
h_transfer:
h_rcvrelease:
h_indicon:
h_indicoff:
h_reset:
        xor     ax,ax                   ; NDIS success
        retf

;=====================================================================
; System Request handler.  The Protocol Manager calls this (FAR) to drive
; the module through binding.  Opcode word at [bp+08]; opcode 2 =
; InitiateBind (accept it); others answer the NE2000 convention (0/>5 ->
; 7 invalid, else -> 8).  Cleans 7 words of arguments (retf 0Eh).
;=====================================================================
sysreq:
        push    bp
        mov     bp,sp
        mov     ax,[bp+08h]             ; opcode
        cmp     ax,2
        jne     sr_other
        xor     ax,ax                   ; InitiateBind: success
        jmp     sr_ret
sr_other:
        or      ax,ax
        jz      sr_inval
        cmp     ax,5
        ja      sr_inval
        mov     ax,8                    ; a recognised-but-unhandled request
        jmp     sr_ret
sr_inval:
        mov     ax,7                    ; invalid request
sr_ret:
        pop     bp
        retf    0Eh

resident_end:                           ; everything above stays resident

;=====================================================================
; INIT (device command 0), discardable.  Opens the Protocol Manager,
; registers the MAC, and reports the resident size.  ES:BX -> request pkt.
;=====================================================================

;--- the Protocol Manager IOCTL request block (14 bytes) ------------
req_block:
req_opcode      dw      0               ; +00 opcode
req_status      dw      0               ; +02 status (returned)
req_p1_off      dw      0               ; +04 Pointer1 offset
req_p1_seg      dw      0               ; +06 Pointer1 segment
req_p2_off      dw      0               ; +08 Pointer2 offset
req_p2_seg      dw      0               ; +0A Pointer2 segment
req_resv        dw      0               ; +0C reserved

binding_list:
        dw      0                       ; empty binding list (a MAC binds to no one)

pm_handle       dw      0
pm_name         db      '\DEV\PROTMAN$', 0

msg_ok          db      13,10,'PMDOSND: MAC registered with Protocol Manager',13,10,'$'
msg_nopm        db      13,10,'PMDOSND: cannot open Protocol Manager',13,10,'$'
msg_noreg       db      13,10,'PMDOSND: Protocol Manager would not register the MAC',13,10,'$'

do_init:
        ; open \DEV\PROTMAN$
        mov     ax,3D00h
        mov     dx,OFFSET pm_name
        int     21h
        jc      di_nopm
        mov     [pm_handle],ax

        ; GetProtocolManagerInfo (opcode 1) - establishes the session
        mov     word ptr [req_opcode],1
        mov     bx,[pm_handle]
        mov     ax,4402h
        mov     cx,0Eh
        mov     dx,OFFSET req_block
        int     21h
        jc      di_noreg_close
        cmp     word ptr [req_status],0
        jne     di_noreg_close

        ; patch our structures' far-pointer segments to our load segment
        call    fill_segs

        ; RegisterModule (opcode 2): Pointer1 = Common Characteristics,
        ; Pointer2 = our (empty) binding list.
        mov     word ptr [req_opcode],2
        mov     word ptr [req_p1_off],OFFSET common_chars
        mov     [req_p1_seg],cs
        mov     word ptr [req_p2_off],OFFSET binding_list
        mov     [req_p2_seg],cs
        mov     bx,[pm_handle]
        mov     ax,4402h
        mov     cx,0Eh
        mov     dx,OFFSET req_block
        int     21h
        jc      di_noreg_close
        cmp     word ptr [req_status],0
        jne     di_noreg_close

        ; close the Protocol Manager handle
        mov     ah,3Eh
        mov     bx,[pm_handle]
        int     21h

        ; success: stay resident, but keep ONLY up to resident_end.  DOS
        ; pre-sets the break address to the end of the whole block the
        ; driver was given (so an INIT that forgets to set it keeps all of
        ; it); left alone, this shim would hold every spare byte of
        ; conventional memory and the next CONFIG.SYS driver - IFSHLP -
        ; would fail for want of it.  resident_end is above every
        ; structure the Protocol Manager holds a pointer into, so trimming
        ; there keeps all of them and discards only the INIT tail below.
        mov     dx,OFFSET msg_ok
        mov     ah,9
        int     21h
        les     bx,[req_ptr]
        mov     word ptr es:[bx+0Eh],OFFSET resident_end        ; break offset
        mov     es:[bx+10h],cs                                  ; break segment = load segment
        mov     word ptr es:[bx+3],0100h        ; status = done
        ret

di_noreg_close:
        mov     ah,3Eh
        mov     bx,[pm_handle]
        int     21h
        mov     dx,OFFSET msg_noreg
        jmp     di_fail
di_nopm:
        mov     dx,OFFSET msg_nopm
di_fail:
        mov     ah,9
        int     21h
        ; do not stay resident (break address = header start) and report
        ; "done" so the system keeps booting without the network.
        les     bx,[req_ptr]
        mov     word ptr es:[bx+0Eh],0
        mov     es:[bx+10h],cs
        mov     word ptr es:[bx+3],0100h
        ret

;--- fill_segs: set every far-pointer segment word to CS ------------
fill_segs:
        mov     ax,cs
        mov     word ptr [common_chars+22h],ax  ; module DS
        mov     word ptr [common_chars+26h],ax  ; SystemRequest seg
        mov     word ptr [common_chars+2Ah],ax  ; service chars seg
        mov     word ptr [common_chars+2Eh],ax  ; service status seg
        mov     word ptr [common_chars+32h],ax  ; upper dispatch seg
        mov     word ptr [svc_status+0Eh],ax    ; SystemRequest seg (status)
        mov     word ptr [upper_disp+2],ax      ; back-pointer seg
        mov     word ptr [upper_disp+6],ax      ; TransmitChain seg
        mov     word ptr [upper_disp+0Ah],ax    ; TransferData seg
        mov     word ptr [upper_disp+0Eh],ax    ; ReceiveRelease seg
        mov     word ptr [upper_disp+12h],ax    ; IndicationOn seg
        mov     word ptr [upper_disp+16h],ax    ; IndicationOff seg
        mov     word ptr [upper_disp+1Ah],ax    ; 6th handler seg
        ret

_TEXT   ENDS
        END
