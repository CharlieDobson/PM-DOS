;=====================================================================
; VDMA.INC - the DMA controller a DOS box believes in
;
; A DRIVER THAT PROGRAMS THE 8237 HANDS IT A PHYSICAL ADDRESS, worked
; out the only way a real-mode program can: segment times sixteen.
; The page tables make that a lie in two places (VDS.INC's header has
; them both): UPPER MEMORY, which is pages from somewhere above 1MB that
; only appear at C8000h, and THE EXPANDED MEMORY PAGE FRAME, four of
; those that move.  A sound driver loaded high, handing the controller
; its own buffer, sent the card into the ROM area - or to wherever
; C8000h really was - and the program heard nothing, or heard memory.
; VDS is the cure for a driver that asks; this is the cure for every
; other one, and it is the same as EMM386's.
;
; THE PORTS ARE THE MONITOR'S: both controllers' registers and seven
; of the page registers, claimed in the TSS bitmap.  What a box writes
; to a channel's address, count and page is KEPT HERE and not sent on,
; and the real channel is programmed when the box UNMASKS it, which is
; the moment a transfer can start and therefore the first moment the
; whole address is known.  Then:
;
;   - the box's buffer, linear, is looked up in the page tables
;     (vd_run).  Where it is is where it was, and the box's own numbers
;     go to the controller - every buffer in conventional memory.
;   - somewhere else, but in one piece below 16MB and across no line
;     the controller cannot cross (64K for channels 0-3, 128K for 5-7):
;     the controller is given where it really is.
;   - neither: THE BOUNCE BUFFER (DMA_BUF, 32K below 16MB).  A transfer
;     from memory is copied into it first; a transfer into memory is
;     copied out of it as it goes - whenever the box looks at a DMA
;     port, and before any interrupt is delivered to it, which between
;     them are every moment a driver can find out a transfer has ended.
;     One channel has the buffer at a time.
;
; AND READING IT BACK TELLS THE BOX ITS OWN STORY.  A current address
; read is turned back into the box's address, so a sound driver
; watching how far its card has got through the buffer sees its own
; buffer.  The count is the same in both pictures.  The terminal-count
; bits in the status register are kept until the box reads them,
; because the kernel reads the real one too and reading it clears them.
;
; THE FLIP-FLOP THE BOX TOGGLES IS A COPY.  Every register it selects
; between is stood in for, so the real one is the kernel's alone and is
; cleared before every pair of bytes the kernel reads or writes.
;
; NOT DONE.  An auto-initialising transfer - a sound card's ring - that
; would need the bounce buffer is REFUSED rather than copied a piece at
; a time behind the card's back: the channel stays masked, the card
; gets nothing, and the count of refusals says so (BUS_DMASTAT).  So is
; a buffer the box could not write itself, which is the other half of
; the point: a DMA channel was a way for any program to write anywhere
; in the machine, the kernel included.  And a channel the kernel has
; put into cascade mode for a bus-master card (vdma_kept) is not the
; box's to mask, unmask or reprogram at all.
;
; Included by IOPM.ASM, after VDS.INC, whose vd_run it uses.
;=====================================================================

; One channel, as the box programmed it and as the controller was
VDC_ADDR        EQU     0       ; dw - base address, as the box wrote it
VDC_COUNT       EQU     2       ; dw - count, likewise
VDC_PAGE        EQU     4       ; db - page register, likewise
VDC_MODE        EQU     5       ; db - mode register, likewise
VDC_FLAGS       EQU     6       ; db - VCF_*
VDC_NOXL        EQU     7       ; db - VDS 810Bh's count: not translated
VDC_LIN         EQU     8       ; dd - the box's buffer, linear, lowest
                                ;      byte (the transfer's first byte is
                                ;      the highest if it counts down)
VDC_REAL        EQU     12      ; dd - where the controller started,
                                ;      physical
VDC_BYTES       EQU     16      ; dd - the transfer's length
VDC_DONE        EQU     20      ; dd - bytes copied out of the bounce
                                ;      buffer so far
VDC_LATCH       EQU     24      ; dw - a register being read a byte at a
                                ;      time
VDC_SIZE        EQU     28

VCF_DIRTY       EQU     01h     ; written since the controller was given it
VCF_MOVED       EQU     02h     ; the controller's address is not the box's
VCF_BOUNCE      EQU     04h     ; ...it is the bounce buffer's, which this
                                ; channel has
VCF_REFUSED     EQU     08h     ; it could not be done, and is masked

; The mode register
DMM_TYPE        EQU     0Ch     ; bits 3-2, what moves:
DMM_VERIFY      EQU     00h     ;   nothing
DMM_WRITE       EQU     04h     ;   the device writes memory
DMM_READ        EQU     08h     ;   the device reads memory
DMM_AUTO        EQU     10h     ; starts again at the end
DMM_DOWN        EQU     20h     ; counts the address down
DMM_CASCADE     EQU     0C0h    ; bits 7-6 both set: a bus master's

; What a port is, as vdma_which says it in AH
VDP_ADDR        EQU     0
VDP_COUNT       EQU     1
VDP_PAGE        EQU     2
VDP_STATUS      EQU     3       ; read: status; written: command
VDP_REQUEST     EQU     4
VDP_SMASK       EQU     5
VDP_MODE        EQU     6
VDP_CLRFF       EQU     7
VDP_MCLEAR      EQU     8       ; written: master clear; read: temporary
VDP_CLRMASK     EQU     9
VDP_WMASK       EQU     10

;=====================================================================
; vdma_claim - from v86_init: the ports become the monitor's.
;=====================================================================
vdma_claim:
        pushad
        mov     byte ptr [vdma_ch + 4 * VDC_SIZE + VDC_MODE],DMM_CASCADE
        xor     edx,edx                 ; 00h-0Fh, the first controller
vcl_first:                              ; (v86_claim spends AL: the port
        mov     eax,edx                 ; is kept in EDX)
        call    v86_claim
        inc     edx
        cmp     edx,10h
        jb      vcl_first
        mov     edx,0C0h                ; C0h-DEh, the second, on the even
vcl_second:                             ; ports
        mov     eax,edx
        call    v86_claim
        add     edx,2
        cmp     edx,0E0h
        jb      vcl_second
        mov     esi,OFFSET vdma_pgport  ; and the seven page registers
        mov     edx,8
vcl_page:
        movzx   eax,byte ptr [esi]
        inc     esi
        or      eax,eax
        jz      vcl_pnext               ; channel 4 has none
        call    v86_claim
vcl_pnext:
        dec     edx
        jnz     vcl_page
        popad
        ret

;=====================================================================
; vdma_keep - AH = a bit for each channel the kernel has given a bus-
;             master card (AHA154X.INC): not the box's from here on.
;=====================================================================
vdma_keep:
        or      byte ptr [vdma_kept],ah
        ret

;---------------------------------------------------------------------
; vdma_which - DX = a port.  CF clear if it is one of the controllers':
;              AH = VDP_*, and EBX = the channel for VDP_ADDR, VDP_COUNT
;              and VDP_PAGE, the controller (0 or 1) for the rest.
;---------------------------------------------------------------------
vdma_which:
        cmp     dx,10h
        jb      vdw_first
        cmp     dx,0C0h
        jb      vdw_page
        cmp     dx,0E0h
        jae     vdw_no
        test    dl,1
        jnz     vdw_no
        movzx   ebx,dx
        sub     ebx,0C0h
        shr     ebx,1                   ; its register, 0-15
        cmp     ebx,8
        jb      vdw_chan2
        mov     ah,byte ptr [vdma_regs + ebx - 8]
        mov     ebx,1
        clc
        ret
vdw_chan2:
        mov     ah,bl
        and     ah,1
        shr     ebx,1
        add     ebx,4
        clc
        ret
vdw_first:
        movzx   ebx,dx
        cmp     ebx,8
        jb      vdw_chan1
        mov     ah,byte ptr [vdma_regs + ebx - 8]
        xor     ebx,ebx
        clc
        ret
vdw_chan1:
        mov     ah,bl
        and     ah,1
        shr     ebx,1
        clc
        ret
vdw_page:
        push    ecx
        xor     ebx,ebx
vdw_pg:
        movzx   ecx,byte ptr [vdma_pgport + ebx]
        cmp     dx,cx
        je      vdw_pgot
        inc     ebx
        cmp     ebx,8
        jb      vdw_pg
        pop     ecx
vdw_no:
        stc
        ret
vdw_pgot:
        pop     ecx
        mov     ah,VDP_PAGE
        clc
        ret

;=====================================================================
; vdma_out - DX = a port, AL = the byte, from vio_out.  Everything
;            preserved.  A port that is not a controller's goes to the
;            hardware.
;=====================================================================
vdma_out:
        pushad
        call    vdma_which
        jc      vdo_pass
        call    vdma_poll               ; a transfer through the bounce
        movzx   ecx,ah                  ; buffer comes up to date first
        jmp     dword ptr [vdo_table + ecx*4]
vdo_pass:
        out     dx,al
vdo_out:
        popad
        ret

;--- the base address, the count, the page: kept, and the channel is
;--- the controller's again once all of it is in, if it is unmasked
vdo_addr:
        call    vdma_rec
        movzx   ecx,byte ptr [vdma_ff + edi]
        mov     byte ptr [esi + VDC_ADDR + ecx],al
        jmp     short vdo_byte
vdo_count:
        call    vdma_rec
        movzx   ecx,byte ptr [vdma_ff + edi]
        mov     byte ptr [esi + VDC_COUNT + ecx],al
vdo_byte:
        xor     byte ptr [vdma_ff + edi],1
        or      byte ptr [esi + VDC_FLAGS],VCF_DIRTY
        cmp     byte ptr [vdma_ff + edi],0
        jne     vdo_out                 ; half of it
        jmp     short vdo_live
vdo_page:
        call    vdma_rec
        mov     byte ptr [esi + VDC_PAGE],al
        or      byte ptr [esi + VDC_FLAGS],VCF_DIRTY
vdo_live:
        bt      dword ptr [vdma_kept],ebx
        jc      vdo_out                 ; the kernel's: never the box's
        bt      dword ptr [vdma_mask],ebx
        jc      vdo_out                 ; masked: it waits for the unmask
        call    vdma_unmask             ; reprogrammed while it runs, as
        jmp     vdo_out                 ; the part would be

vdo_status:                             ; the command register
        out     dx,al
        jmp     vdo_out

vdo_request:
        call    vdma_chan
        jc      vdo_out                 ; the kernel's channel
        test    al,04h
        jz      vdo_reqout
        push    eax
        call    vdma_program            ; a request is a start, too
        pop     eax
        jc      vdo_out
vdo_reqout:
        out     dx,al
        jmp     vdo_out

vdo_smask:
        call    vdma_chan
        jc      vdo_out
        test    al,04h
        jz      vdo_unmask
        call    vdma_shut
        jmp     vdo_out
vdo_unmask:
        call    vdma_unmask
        jmp     vdo_out

vdo_mode:
        call    vdma_chan
        jc      vdo_out
        mov     byte ptr [esi + VDC_MODE],al
        out     dx,al
        jmp     vdo_out

vdo_clrff:
        mov     byte ptr [vdma_ff + ebx],0
        jmp     vdo_out

;--- a master clear: every channel masked, the flip-flop cleared, the
;--- status forgotten.  Done a channel at a time, so that a channel the
;--- kernel keeps is not masked with the others.
vdo_mclear:
        mov     byte ptr [vdma_ff + ebx],0
        mov     byte ptr [vdma_stat + ebx],0
        mov     ecx,0Fh
        jmp     short vdo_masks

vdo_clrmask:
        xor     ecx,ecx
        jmp     short vdo_masks

vdo_wmask:
        movzx   ecx,al
vdo_masks:
        shl     ebx,2                   ; the controller's first channel
        mov     edi,4
vdm_each:
        bt      dword ptr [vdma_kept],ebx
        jc      vdm_next
        push    ecx
        push    edx
        push    edi
        shr     ecx,1
        jc      vdm_shut
        call    vdma_unmask
        jmp     short vdm_did
vdm_shut:
        call    vdma_shut
vdm_did:
        pop     edi
        pop     edx
        pop     ecx
vdm_next:
        shr     ecx,1
        inc     ebx
        dec     edi
        jnz     vdm_each
        jmp     vdo_out

;=====================================================================
; vdma_in - DX = a port -> AL, from vio_in.  Everything else preserved.
;=====================================================================
vdma_in:
        pushad
        call    vdma_which
        jc      vdi_pass
        call    vdma_poll
        cmp     ah,VDP_ADDR
        je      vdi_reg
        cmp     ah,VDP_COUNT
        je      vdi_reg
        cmp     ah,VDP_PAGE
        je      vdi_page
        cmp     ah,VDP_STATUS
        je      vdi_status
vdi_pass:
        in      al,dx
vdi_out:
        mov     byte ptr [esp + 28],al  ; into the AL popad puts back
        popad
        ret

vdi_page:
        call    vdma_rec
        mov     al,byte ptr [esi + VDC_PAGE]
        jmp     short vdi_out

;--- the status: what is asked for now, and every terminal count since
;--- the box last looked, which reading it forgets
vdi_status:
        in      al,dx
        mov     ah,al
        and     ah,0Fh
        or      ah,byte ptr [vdma_stat + ebx]
        mov     byte ptr [vdma_stat + ebx],0
        and     al,0F0h
        or      al,ah
        jmp     short vdi_out

;--- a current address or count, a byte at a time.  The first byte
;--- works the whole register out and keeps it, the second is the rest
;--- of the same number.
vdi_reg:
        call    vdma_rec
        cmp     byte ptr [vdma_ff + edi],0
        jne     vdi_second
        mov     byte ptr [vdma_ff + edi],1
        mov     cl,ah
        test    byte ptr [esi + VDC_FLAGS],VCF_DIRTY
        jz      vdi_live
        mov     ax,word ptr [esi + VDC_ADDR]    ; written and not started:
        cmp     cl,VDP_ADDR                     ; what was written
        je      vdi_keep
        mov     ax,word ptr [esi + VDC_COUNT]
        jmp     short vdi_keep
vdi_live:
        push    ecx
        call    vdma_cur                ; EAX = address, ECX = count
        mov     edx,ecx
        pop     ecx
        cmp     cl,VDP_ADDR
        jne     vdi_cnt
        test    byte ptr [esi + VDC_FLAGS],VCF_MOVED
        jz      vdi_keep
        mov     edx,dword ptr [esi + VDC_REAL]  ; how far the controller has
        cmp     ebx,4                           ; got, from where it began,
        jb      vdi_byte                        ; is how far the box's has
        shr     edx,1
vdi_byte:
        sub     ax,dx
        add     ax,word ptr [esi + VDC_ADDR]
        jmp     short vdi_keep
vdi_cnt:
        mov     eax,edx
vdi_keep:
        mov     word ptr [esi + VDC_LATCH],ax
        jmp     vdi_out
vdi_second:
        mov     byte ptr [vdma_ff + edi],0
        mov     al,byte ptr [esi + VDC_LATCH + 1]
        jmp     vdi_out

;---------------------------------------------------------------------
; vdma_rec - EBX = a channel -> ESI -> its record, EDI = its controller.
; vdma_chan - EBX = a controller, AL = a byte naming one of its channels
;             in bits 1-0 -> EBX = the channel, ESI, EDI as vdma_rec.
;             CF if it is one the kernel keeps.
;---------------------------------------------------------------------
vdma_chan:
        shl     ebx,2
        movzx   ecx,al
        and     ecx,3
        add     ebx,ecx
        call    vdma_rec
        bt      dword ptr [vdma_kept],ebx
        ret
vdma_rec:
        mov     esi,ebx
        imul    esi,esi,VDC_SIZE
        add     esi,OFFSET vdma_ch
        mov     edi,ebx
        shr     edi,2
        ret

;=====================================================================
; vdma_unmask - EBX = a channel the box has unmasked.  The controller is
;               given the transfer (vdma_program) and the channel goes
;               live - or, if that cannot be done, stays masked.
; vdma_shut   - EBX = a channel the box has masked.  Masked, and a
;               transfer through the bounce buffer is finished.
;
; Both write the real single-channel mask register, whichever register
; the box used, and keep the box's picture of the mask in vdma_mask.
;=====================================================================
vdma_unmask:
        pushad
        btr     dword ptr [vdma_mask],ebx
        call    vdma_program
        jc      vum_no
        call    vdma_rec
        and     byte ptr [esi + VDC_FLAGS],NOT VCF_REFUSED
        mov     al,bl
        and     al,3                    ; bit 2 clear: unmask
        call    vdma_smout
        popad
        ret
vum_no:
        call    vdma_rec
        or      byte ptr [esi + VDC_FLAGS],VCF_REFUSED
        inc     dword ptr [vdma_nrefused]
        mov     al,bl
        and     al,3
        or      al,04h
        call    vdma_smout
        popad
        ret

vdma_shut:
        pushad
        bts     dword ptr [vdma_mask],ebx
        mov     al,bl
        and     al,3
        or      al,04h
        call    vdma_smout
        call    vdma_rec
        call    vdma_copyback
        cmp     byte ptr [vdma_bowner],0
        je      vsh_out
        movzx   eax,byte ptr [vdma_bowner]
        dec     eax
        cmp     eax,ebx
        jne     vsh_out
        call    vdma_release
vsh_out:
        popad
        ret

;--- vdma_smout - AL = a single-mask byte for channel EBX's controller
vdma_smout:
        push    edx
        mov     dx,0Ah
        cmp     ebx,4
        jb      vso_first
        mov     dx,0D4h
vso_first:
        out     dx,al
        pop     edx
        ret

;=====================================================================
; vdma_program - EBX = a channel about to be able to move data.  Give
;                the controller the box's transfer, as it can reach it.
;                CF if it cannot be done.
;
; A channel the box has not written to since the last time is left as
; it is: unmasking it again carries on from where it stopped, as the
; part does.  So does one in cascade mode, which has no address.
;=====================================================================
vdma_program:
        pushad
        call    vdma_rec
        test    byte ptr [esi + VDC_FLAGS],VCF_DIRTY
        jz      vp_ok
        movzx   eax,byte ptr [vdma_bowner]      ; a new transfer: whatever the
        dec     eax                             ; last one had of the bounce
        cmp     eax,ebx                         ; buffer is given up
        jne     vp_nobuf
        call    vdma_release
vp_nobuf:
        mov     al,byte ptr [esi + VDC_MODE]
        and     al,DMM_CASCADE
        cmp     al,DMM_CASCADE
        je      vp_clean

        ; WHERE AND HOW BIG.  A byte channel's address is its page and
        ; sixteen bits; a word channel's page loses bit 0 and its sixteen
        ; bits count words.
        movzx   ecx,word ptr [esi + VDC_COUNT]
        inc     ecx                     ; transfers: 1 to 65536
        movzx   eax,byte ptr [esi + VDC_PAGE]
        movzx   edx,word ptr [esi + VDC_ADDR]
        cmp     ebx,4
        jb      vp_bytes
        and     eax,0FEh
        shl     edx,1
        shl     ecx,1                   ; ECX = bytes
vp_bytes:
        shl     eax,16
        or      eax,edx                 ; EAX = the first byte it moves
        mov     dword ptr [vp_first],eax
        mov     dword ptr [esi + VDC_BYTES],ecx
        test    byte ptr [esi + VDC_MODE],DMM_DOWN
        jz      vp_up
        sub     eax,ecx                 ; counting down, the buffer ends
        add     eax,1                   ; at the first byte (of the last
        cmp     ebx,4                   ; word, on a word channel)
        jb      vp_up
        add     eax,1
vp_up:
        mov     dword ptr [esi + VDC_LIN],eax

        cmp     byte ptr [esi + VDC_NOXL],0
        jne     vp_given                ; VDS 810Bh: it says what it means
        mov     al,byte ptr [esi + VDC_MODE]
        and     al,DMM_TYPE
        cmp     al,DMM_VERIFY
        je      vp_given                ; nothing in memory moves

        ; THE BOX'S TO TOUCH, or not at all.  Ring 0 is what the
        ; controller writes as, and the kernel is what it would reach.
        mov     eax,dword ptr [esi + VDC_LIN]
        mov     ecx,dword ptr [esi + VDC_BYTES]
        call    usr_wok
        jc      vp_refuse

        ; WHERE IT REALLY IS.
        mov     ebx,dword ptr [esi + VDC_LIN]
        mov     ecx,dword ptr [esi + VDC_BYTES]
        call    vd_run                  ; EAX = physical, ECX = in one piece
        jc      vp_refuse
        cmp     ecx,dword ptr [esi + VDC_BYTES]
        jb      vp_bounce
        cmp     eax,dword ptr [esi + VDC_LIN]
        je      vp_given                ; where it says: nothing to do
        mov     ecx,dword ptr [esi + VDC_BYTES]
        lea     edx,[eax + ecx]
        cmp     edx,01000000h
        ja      vp_bounce               ; beyond what the bus can address
        dec     edx
        xor     edx,eax
        and     edx,0FFFF0000h          ; the line it may not cross: 64K,
        mov     ebx,dword ptr [esp + 16]        ; the caller's EBX: the channel
        cmp     ebx,4                   ; or 128K for a word channel
        jb      vp_line
        and     edx,0FFFE0000h
vp_line:
        or      edx,edx
        jnz     vp_bounce
        or      byte ptr [esi + VDC_FLAGS],VCF_MOVED
        jmp     short vp_load

        ; NOT WHERE THE CONTROLLER CAN REACH IT: through the bounce buffer,
        ; if it is short enough, if it ends, and if the buffer is free.
vp_bounce:
        mov     ebx,dword ptr [esp + 16]
        cmp     dword ptr [esi + VDC_BYTES],DMA_BUFLEN
        ja      vp_refuse
        test    byte ptr [esi + VDC_MODE],DMM_AUTO
        jnz     vp_refuse
        movzx   eax,byte ptr [vdma_bowner]
        or      eax,eax
        jz      vp_take
        dec     eax
        cmp     eax,ebx
        je      vp_take
        bt      dword ptr [vdma_mask],eax       ; its owner is masked and
        jnc     vp_refuse                       ; so finished with it
vp_take:
        lea     eax,[ebx + 1]
        mov     byte ptr [vdma_bowner],al
        or      byte ptr [esi + VDC_FLAGS],VCF_MOVED OR VCF_BOUNCE
        mov     dword ptr [esi + VDC_DONE],0
        mov     al,byte ptr [esi + VDC_MODE]
        and     al,DMM_TYPE
        cmp     al,DMM_READ
        jne     vp_bounced
        push    esi                             ; the device reads it: it
        mov     ecx,dword ptr [esi + VDC_BYTES] ; goes in now
        mov     esi,dword ptr [esi + VDC_LIN]
        mov     edi,DMA_BUF
        cld
        rep     movsb
        pop     esi
vp_bounced:
        mov     eax,DMA_BUF                     ; and the transfer starts as
        add     eax,dword ptr [vp_first]        ; far into it as it starts
        sub     eax,dword ptr [esi + VDC_LIN]   ; into the box's buffer
        jmp     short vp_load2

        ; AS THE BOX GAVE IT, which is right everywhere it means what it
        ; says - conventional memory and the identity above 1MB.
vp_given:
        and     byte ptr [esi + VDC_FLAGS],NOT (VCF_MOVED OR VCF_BOUNCE)
        mov     eax,dword ptr [vp_first]
        jmp     short vp_load2
vp_load:
        add     eax,dword ptr [vp_first]        ; physical of the buffer, and
        sub     eax,dword ptr [esi + VDC_LIN]   ; the first byte as far in
vp_load2:
        mov     ebx,dword ptr [esp + 16]
        mov     dword ptr [esi + VDC_REAL],eax
        call    vdma_load
vp_clean:
        and     byte ptr [esi + VDC_FLAGS],NOT VCF_DIRTY
vp_ok:
        popad
        clc
        ret
vp_refuse:
        popad
        stc
        ret

;---------------------------------------------------------------------
; vdma_load - EBX = a channel, ESI -> its record, EAX = the physical
;             address the transfer starts at.  The controller's
;             address, count and page for it.  Everything preserved.
;---------------------------------------------------------------------
vdma_load:
        pushad
        mov     ecx,eax
        movzx   edx,byte ptr [vdma_pgport + ebx]
        mov     ebp,edx                         ; EBP = its page register
        cmp     ebx,4
        jb      vld_first
        shr     eax,1                           ; words, and the page's bit
        and     ecx,0FE0000h                    ; 0 is not the page's
        lea     edx,[ebx - 4]
        shl     edx,2
        add     edx,0C0h                        ; DX = the address register
        mov     edi,0D8h                        ; EDI = the flip-flop's
        mov     ebx,2                           ; EBX = to the count register
        jmp     short vld_go
vld_first:
        lea     edx,[ebx + ebx]
        mov     edi,0Ch
        mov     ebx,1
vld_go:
        xchg    edx,edi
        out     dx,al                           ; the flip-flop cleared
        xchg    edx,edi
        out     dx,al                           ; the address, low byte
        mov     al,ah
        out     dx,al                           ; ...and high
        add     edx,ebx
        mov     ax,word ptr [esi + VDC_COUNT]
        out     dx,al
        mov     al,ah
        out     dx,al
        mov     eax,ecx
        shr     eax,16
        mov     edx,ebp
        out     dx,al                           ; the page
        popad
        ret

;---------------------------------------------------------------------
; vdma_cur - EBX = a channel -> EAX = the controller's current address
;            register, ECX = its current count register, 16 bits each.
;---------------------------------------------------------------------
vdma_cur:
        push    edx
        push    edi
        cmp     ebx,4
        jb      vcu_first
        lea     edx,[ebx - 4]
        shl     edx,2
        add     edx,0C0h
        mov     edi,2
        mov     al,0
        out     0D8h,al
        jmp     short vcu_go
vcu_first:
        lea     edx,[ebx + ebx]
        mov     edi,1
        mov     al,0
        out     0Ch,al
vcu_go:
        in      al,dx
        mov     ah,al
        in      al,dx
        xchg    al,ah
        movzx   ecx,ax                  ; for now
        add     edx,edi
        in      al,dx
        mov     ah,al
        in      al,dx
        xchg    al,ah
        xchg    cx,ax                   ; AX = address, CX = count
        movzx   eax,ax
        movzx   ecx,cx
        pop     edi
        pop     edx
        ret

;=====================================================================
; vdma_poll - a transfer into memory through the bounce buffer: what
;             the device has written so far goes where the box will
;             look for it.  Before the box sees any port of the
;             controllers, and before any interrupt reaches it (VPIC.INC,
;             vpc_take).  Everything preserved.
;=====================================================================
vdma_poll:
        cmp     byte ptr [vdma_bowner],0
        je      vpl_out
        pushad
        movzx   ebx,byte ptr [vdma_bowner]
        dec     ebx
        call    vdma_rec
        call    vdma_copyback
        popad
vpl_out:
        ret

;---------------------------------------------------------------------
; vdma_copyback - EBX = a channel, ESI -> its record.  If it is moving
;                 data into memory through the bounce buffer, what has
;                 arrived is copied on to the box's buffer; when all of
;                 it has, the buffer is given up.  A transfer the other
;                 way gives the buffer up when it is over too.
;---------------------------------------------------------------------
vdma_copyback:
        test    byte ptr [esi + VDC_FLAGS],VCF_BOUNCE
        jz      vcb_out
        pushad
        call    vdma_cur                ; ECX = transfers still to go, less
        inc     ecx                     ; one - FFFFh once there are none
        and     ecx,0FFFFh
        cmp     ebx,4
        jb      vcb_bytes
        shl     ecx,1
vcb_bytes:
        mov     eax,dword ptr [esi + VDC_BYTES]
        sub     eax,ecx                 ; EAX = bytes moved so far
        jb      vcb_done                ; nonsense: the box reprogrammed it
        mov     dl,byte ptr [esi + VDC_MODE]
        and     dl,DMM_TYPE
        cmp     dl,DMM_WRITE
        jne     vcb_end
        test    byte ptr [esi + VDC_MODE],DMM_DOWN
        jz      vcb_up
        or      ecx,ecx                 ; counting down: the whole of it,
        jnz     vcb_done                ; at the end
        mov     ecx,eax
        mov     edi,dword ptr [esi + VDC_LIN]
        mov     esi,DMA_BUF
        jmp     short vcb_copy
vcb_up:
        mov     edx,dword ptr [esi + VDC_DONE]
        mov     dword ptr [esi + VDC_DONE],eax
        sub     eax,edx
        jbe     vcb_end
        push    ecx
        mov     ecx,eax
        mov     edi,dword ptr [esi + VDC_LIN]
        add     edi,edx
        lea     esi,[edx + DMA_BUF]
        cld
        rep     movsb
        pop     ecx
        jmp     short vcb_end
vcb_copy:
        cld
        rep     movsb
        xor     ecx,ecx
vcb_end:
        or      ecx,ecx
        jnz     vcb_done
        call    vdma_release            ; all of it: the buffer is free
vcb_done:
        popad
vcb_out:
        ret

;--- vdma_release - the bounce buffer is nobody's again
vdma_release:
        push    eax
        push    esi
        movzx   eax,byte ptr [vdma_bowner]
        or      eax,eax
        jz      vrl_out
        dec     eax
        imul    esi,eax,VDC_SIZE
        and     byte ptr [vdma_ch + esi + VDC_FLAGS],NOT VCF_BOUNCE
        mov     byte ptr [vdma_bowner],0
vrl_out:
        pop     esi
        pop     eax
        ret

;=====================================================================
; vdma_noxl - VDS 810Bh and 810Ch.  EBX = a channel, AL = 1 to stop
;             translating it, 0 to start again.  CF and AL = the VDS
;             error if that cannot be done; otherwise ZF set if it is
;             being translated again.
;=====================================================================
vdma_noxl:
        cmp     ebx,8
        jae     vnx_badch
        push    esi
        push    edi
        call    vdma_rec
        or      al,al
        jz      vnx_on
        cmp     byte ptr [esi + VDC_NOXL],0FFh
        je      vnx_over
        inc     byte ptr [esi + VDC_NOXL]
        or      byte ptr [esi + VDC_FLAGS],VCF_DIRTY
        jmp     short vnx_ok
vnx_on:
        cmp     byte ptr [esi + VDC_NOXL],0
        je      vnx_under
        dec     byte ptr [esi + VDC_NOXL]
        or      byte ptr [esi + VDC_FLAGS],VCF_DIRTY
vnx_ok:
        cmp     byte ptr [esi + VDC_NOXL],0     ; ZF: translated
        pop     edi
        pop     esi
        clc
        ret
vnx_over:
        mov     al,0Dh
        jmp     short vnx_fail
vnx_under:
        mov     al,0Eh
vnx_fail:
        pop     edi
        pop     esi
        stc
        ret
vnx_badch:
        mov     al,0Ch
        stc
        ret

;=====================================================================
; vdma_stats - BUS_DMASTAT.  -> EAX = transfers refused, EBX = the
;              channels the kernel keeps, ECX = the bounce buffer's
;              size, EDX = the channel that has it plus one, or 0.
;=====================================================================
vdma_stats:
        mov     eax,dword ptr [vdma_nrefused]
        movzx   ebx,byte ptr [vdma_kept]
        mov     ecx,DMA_BUFLEN
        movzx   edx,byte ptr [vdma_bowner]
        clc
        ret

;=====================================================================
; State
;=====================================================================
vdo_table       dd      vdo_addr, vdo_count, vdo_page, vdo_status
                dd      vdo_request, vdo_smask, vdo_mode, vdo_clrff
                dd      vdo_mclear, vdo_clrmask, vdo_wmask
vdma_regs       db      VDP_STATUS, VDP_REQUEST, VDP_SMASK, VDP_MODE
                db      VDP_CLRFF, VDP_MCLEAR, VDP_CLRMASK, VDP_WMASK
vdma_pgport     db      87h, 83h, 81h, 82h, 0, 8Bh, 89h, 8Ah    ; a channel's
                                                                ; page register
vdma_ch         db      8 * VDC_SIZE dup(0)
vdma_mask       dd      0EFh            ; the box's picture: channel 4, the
                                        ; cascade, open and the rest masked
vdma_nrefused   dd      0
vp_first        dd      0
vdma_ff         db      0, 0            ; the box's flip-flops
vdma_stat       db      0, 0            ; terminal counts the box has not
                                        ; read yet
vdma_kept       db      10h             ; the kernel's channels: 4, the
                                        ; cascade, and any bus master's
vdma_bowner     db      0               ; the bounce buffer's channel + 1
