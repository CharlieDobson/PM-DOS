;=====================================================================
; RMUSB.INC - a USB disk served through the Windows-for-Workgroups bridge
;
; When PM-DOS parks under WfW its 32-bit USB driver is kept alive (it
; still owns the controller and the stick stays enumerated - IO\USB.INC
; usb_keepalive), but real-mode RMDOS can reach it only through the
; bridge.  So for a drive the park marked a bridge-served USB disk
; (RMDF_USB -> VF_USB), rd_raw comes HERE instead of to the BIOS: each
; sector is read or written by handing the parked kernel's own block
; layer a block_req across PMDOS.386's PDAPI_BLOCK V86 API (the BR_*
; fields of INC\PMDOS.INC's block_req), one sector a call - the bridge's
; low page holds one.  The bridge masks the PICs and polls the USB
; transfer to the end inside one window, so unlike the redirector this
; does not yield for a PENDING; it only retries when the async tick held
; the bridge that instant (AX = 0FFFFh, CF).
;
; Included by RMDOS.ASM after RMDISK.INC (rd_bios / rd_rw / rd_raw) and
; RMNET2.INC (the INT 2Fh 1684h / 1680h bridge-API idiom).
;=====================================================================
RU_OP           EQU     0               ; block_req, as INC\PMDOS.INC BR_*
RU_DRIVE        EQU     4
RU_LBA          EQU     8
RU_COUNT        EQU     12
RU_STATUS       EQU     16
RU_DATA         EQU     32
RU_SIZE         EQU     544
RU_PDAPI_BLOCK  EQU     2               ; PMDOS.386's block sub-function
RU_DEVID        EQU     504Dh           ; the VxD's device id ("PM")
RU_RETRIES      EQU     6000            ; a wedged bridge gives up, it does not hang

;--- data up front, so the 32-bit stores below know the operand size ---
ru_savelba      dd  0                       ; the LBA of the sector in hand
ru_apitried db  0
ru_apient   label dword
ru_apioff   dw  0
ru_apiseg   dw  0
ru_block    db  RU_SIZE dup(0)          ; one block_req + its sector

;---------------------------------------------------------------------
; rd_usbraw - rd_raw's USB fork.  EAX = first LBA, ECX = sector count,
; ES:BX = buffer, [rd_rw] 0 read / 1 write.  CF on error.  Every register
; kept, as rd_raw keeps them.  One sector a bridge call; ES:BX is brought
; under 16 each sector so no 512-byte sector straddles the 64K line.
;---------------------------------------------------------------------
rd_usbraw:
        push    es
        pushad
rdu_one:
        or      ecx,ecx
        jz      rdu_ok
        ; normalise ES:BX to seg:0..15 so this sector cannot wrap 64K
        push    eax
        mov     ax,es
        movzx   eax,ax
        shl     eax,4
        movzx   edx,bx
        add     eax,edx
        mov     edx,eax
        shr     edx,4
        mov     es,dx
        mov     edx,eax
        and     dx,0Fh
        mov     bx,dx
        pop     eax
        call    ru_1sec                 ; EAX = LBA, ES:BX = one sector
        jc      rdu_fail
        inc     eax
        dec     ecx
        add     bx,512                  ; renormalised at the top of the loop
        jmp     short rdu_one
rdu_ok:
        popad
        pop     es
        clc
        ret
rdu_fail:
        popad
        pop     es
        stc
        ret

;---------------------------------------------------------------------
; ru_1sec - one sector at EAX, ES:BX.  Builds the block_req, ships it
; over the bridge, and moves the 512 bytes the right way.  CF on error.
;---------------------------------------------------------------------
ru_1sec:
        mov     [ru_savelba],eax
        pushad
        ; zero the block_req
        push    es
        push    cs
        pop     es
        mov     di,OFFSET ru_block
        mov     cx,RU_SIZE
        xor     al,al
        cld
        rep     stosb
        pop     es
        ; the fields
        movzx   eax,byte ptr [rd_rw]
        mov     dword ptr [ru_block+RU_OP],eax          ; 0 read, 1 write
        movzx   eax,byte ptr [rd_bios]                  ; the kernel's block number
        mov     dword ptr [ru_block+RU_DRIVE],eax
        mov     eax,[ru_savelba]
        mov     dword ptr [ru_block+RU_LBA],eax
        mov     dword ptr [ru_block+RU_COUNT],1
        ; a write copies the caller's sector into the block first
        cmp     byte ptr [rd_rw],0
        je      ru_fire
        push    ds
        push    es
        mov     ax,es                           ; the caller's buffer segment
        push    cs
        pop     es                              ; ES = ours (the block is here)
        mov     ds,ax                           ; DS:SI = the caller's sector
        mov     si,bx
        mov     di,OFFSET ru_block + RU_DATA
        mov     cx,512
        cld
        rep     movsb
        pop     es
        pop     ds
ru_fire:
        call    ru_req
        jc      ru_fail
        cmp     word ptr [ru_block+RU_STATUS],0
        jne     ru_fail
        cmp     byte ptr [rd_rw],0
        jne     ru_ok                           ; a write has nothing to bring back
        ; a read copies the sector out to the caller (DS = ours, ES = caller's)
        mov     si,OFFSET ru_block + RU_DATA
        mov     di,bx
        mov     cx,512
        cld
        rep     movsb
ru_ok:
        popad
        clc
        ret
ru_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; ru_req - run [ru_block] through PMDOS.386's PDAPI_BLOCK.  -> CF clear
; and ru_block's BR_STATUS = the result; CF set if the bridge API is not
; there or it stayed busy.  The entry is got once, lazily (WIN386 answers
; INT 2Fh 1684h above RMDOS's own hook); a "busy" answer (AX = 0FFFFh, CF)
; means the tick held the bridge - yield this VM's slice and try again.
;---------------------------------------------------------------------
ru_req:
        cmp     word ptr [ru_apiseg],0
        jne     ru_have
        cmp     byte ptr [ru_apitried],0
        jne     ru_noapi
        mov     byte ptr [ru_apitried],1
        push    es
        push    di
        push    bx                      ; the caller's buffer offset must
                                        ; survive this get - 1684h takes BX =
                                        ; the device id, and ru_1sec's read
                                        ; copy uses BX as the destination
        xor     di,di
        mov     es,di
        mov     ax,1684h
        mov     bx,RU_DEVID
        int     2Fh
        mov     ax,es
        or      ax,di
        jz      ru_noapi_pop
        mov     word ptr [ru_apioff],di
        mov     word ptr [ru_apiseg],es
        pop     bx
        pop     di
        pop     es
ru_have:
        push    cx
        mov     cx,RU_RETRIES
ru_poll:
        push    cx
        push    es
        push    cs
        pop     es
        mov     dx,OFFSET ru_block
        mov     ax,RU_PDAPI_BLOCK
        call    dword ptr [ru_apient]
        pop     es
        pop     cx
        jnc     ru_poll_done                    ; CF clear = done (status in the block)
        mov     ax,1680h                        ; busy this instant: release the slice
        int     2Fh
        loop    ru_poll
        pop     cx
        stc
        ret
ru_poll_done:
        pop     cx
        clc
        ret
ru_noapi_pop:
        pop     bx
        pop     di
        pop     es
ru_noapi:
        stc
        ret

IFDEF RMDOS_USBDBG
;---------------------------------------------------------------------
; A SURGICAL PORT-E9h TRACE for the USB-disk directory bug (BC2).  These
; are dropped into the middle of fn4E and dir_first, which are both mid
; INT 21h, so each keeps every register AND the flags.  Built only with
; -dRMDOS_USBDBG; the trace points are IFDEF'd out of every other build.
; The bytes interleave on port E9h with PMDOS.386's own "USB blk" lines.
;---------------------------------------------------------------------
udbg1:                                  ; AL = one character, after a newline
        pushf
        push    ax
        mov     al,10
        out     0E9h,al
        pop     ax
        out     0E9h,al
        popf
        ret
ENDIF
