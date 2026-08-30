;=====================================================================
; PARKWIN.ASM - PARKWIN.COM, the handover, without Windows
;
; PM-DOS answers Windows standard mode's INT 2Fh AX=1605h by LEAVING:
; dropping paging, dropping protected mode, and handing the machine to
; RMDOS.SYS with the broadcasting program resumed on the other side.
; See DOS\WINDOWS.INC and DOS\PARK.INC.
;
; That is a great deal of machinery to first exercise with Windows on
; the end of it, in a graphics mode, with no way to print anything.
; This is the same call from a program that fits on one screen.
;
; WHAT IT PROVES, AND WHY EACH LINE OF IT IS ONE ANSWER.
;
;   SMSW before   PE set - we are a V86 guest under a protected-mode
;                 kernel.  This is the bit DOSX reads and refuses on.
;   the broadcast INT 2Fh AX=1605h with DX bit 0 set, which is what
;                 makes it standard mode's question rather than 386
;                 enhanced mode's.  PM-DOS parks inside this call.
;   SMSW after    PE CLEAR, if the handover worked.  The instruction
;                 after an INT, in a different processor mode from the
;                 one the INT was taken in.
;   CX            zero if we were allowed to start.  Non-zero is the
;                 veto, and then the mode will not have changed either.
;   the printing  every character after the broadcast is an INT 21h
;                 served by RMDOS, so a line that appears at all is a
;                 second DOS answering.
;   4Ch           RMDOS unparks on it, PM-DOS picks the machine back
;                 up, and the box ends.  Getting back to a prompt is
;                 the last check and there is nothing to print for it.
;
; The registers are stamped with a pattern before the call and read
; back after, because the frame the guest is resumed on is built by
; PM-DOS and rebuilt by RMDOS, and a register lost in the middle of
; that is exactly the kind of fault that otherwise shows up much later
; as Windows behaving oddly.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_PW     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_PW, DS:_PW, ES:_PW, SS:_PW
        ORG     0

start:
        ; GIVE THE REST OF THE MACHINE BACK FIRST, and this is not
        ; housekeeping - it is a precondition of the whole test.  DOS
        ; hands a .COM every byte of conventional memory there is, and
        ; the handover has to LOAD RMDOS.SYS into conventional memory:
        ; a program that has not shrunk itself owns the place RMDOS
        ; would go, and the park fails with "not enough memory" on a
        ; machine with 620K free.  Windows does this too - DOSX.EXE's
        ; first act after the broadcast is a 4Ah - which is why it is
        ; the guest's job and not the kernel's.
        mov     bx,1000h                ; 64K, and this file wants 1K
        mov     ah,4Ah
        int     21h                     ; ES is still the PSP here

        mov     dx,OFFSET msg_hi
        call    puts

        ; --- before ---
        mov     dx,OFFSET msg_pre
        call    puts
        call    saymode
        call    sayoem

        ; --- the broadcast ---------------------------------------
        ; ES:BX and DS:SI are the startup-info pointers, which a
        ; resident may fill in and which this one does not use; CX is
        ; the verdict and must go in as zero, because a resident that
        ; consents is one that LEAVES IT ALONE.  DX bit 0 is the mode.
        mov     dx,OFFSET msg_call
        call    puts

        push    ds
        push    es
        xor     ax,ax
        mov     es,ax
        xor     bx,bx
        xor     si,si
        mov     ds,ax
        xor     cx,cx
        mov     di,0A03h                ; the version, as Windows sends it
        mov     bp,0BEEFh               ; and a pattern in the registers
        mov     dx,1                    ; bit 0: standard mode
        mov     ax,1605h
        int     2Fh
        mov     word ptr cs:[got_cx],cx
        mov     word ptr cs:[got_bp],bp
        mov     word ptr cs:[got_di],di
        pop     es
        pop     ds

        ; --- after -----------------------------------------------
        ; From here on every INT 21h is answered by whichever DOS is
        ; underneath us, and the point of the exercise is that this
        ; code cannot tell which.
        mov     dx,OFFSET msg_post
        call    puts
        call    saymode
        call    sayoem

        mov     dx,OFFSET msg_cx
        call    puts
        mov     ax,word ptr cs:[got_cx]
        call    put4
        or      ax,ax
        jz      pw_allowed
        mov     dx,OFFSET msg_vetoed
        call    puts
        jmp     short pw_regs
pw_allowed:
        mov     dx,OFFSET msg_ok
        call    puts

pw_regs:
        mov     dx,OFFSET msg_bp
        call    puts
        mov     ax,word ptr cs:[got_bp]
        call    put4
        cmp     ax,0BEEFh
        je      pw_bpok
        mov     dx,OFFSET msg_bad
        call    puts
        jmp     short pw_di
pw_bpok:
        mov     dx,OFFSET msg_kept
        call    puts
pw_di:
        mov     dx,OFFSET msg_di
        call    puts
        mov     ax,word ptr cs:[got_di]
        call    put4
        cmp     ax,0A03h
        je      pw_diok
        mov     dx,OFFSET msg_bad
        call    puts
        jmp     short pw_bye
pw_diok:
        mov     dx,OFFSET msg_kept
        call    puts

pw_bye:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; sayoem - who answered.  AH=30h returns an OEM byte in BH, and the two
;          DOSes in this machine do not use the same one: PM-DOS says
;          03 and RMDOS says FF.  The mode above proves the processor
;          changed; this proves the DOS did, which is the other half
;          and the half a program would actually notice.
;---------------------------------------------------------------------
sayoem:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     dx,OFFSET msg_oem
        call    puts
        mov     ah,30h
        int     21h
        mov     ax,bx
        call    put4
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
; saymode - "real mode" or "protected/V86", from the machine status
;           word, which is readable from a guest and tells the truth
;           about the whole machine rather than about us.
;---------------------------------------------------------------------
saymode:
        push    ax
        push    dx
        smsw    ax
        test    al,1
        jz      sm_real
        mov     dx,OFFSET msg_prot
        call    puts
        jmp     short sm_out
sm_real:
        mov     dx,OFFSET msg_real
        call    puts
sm_out:
        pop     dx
        pop     ax
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
put1:
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_go
        add     al,7
p1_go:
        mov     dl,al
        call    putc
        pop     ax
        ret

got_cx      dw  0
got_bp      dw  0
got_di      dw  0

msg_hi      db  13,10,'PARKWIN - INT 2Fh AX=1605h, standard mode',13,10,13,10,'$'
msg_pre     db  'before the broadcast: $'
msg_call    db  13,10,'broadcasting...',13,10,'$'
msg_post    db  'after  the broadcast: $'
msg_prot    db  'protected mode or V86 (PE set)',13,10,'$'
msg_real    db  'REAL MODE (PE clear)',13,10,'$'
msg_oem     db  ' the DOS answering says BX = ','$'
msg_crlf    db  13,10,'$'
msg_cx      db  'CX back = $'
msg_ok      db  '  - allowed to start',13,10,'$'
msg_vetoed  db  '  - VETOED, no handover',13,10,'$'
msg_bp      db  'BP  back = $'
msg_di      db  'DI  back = $'
msg_kept    db  '  kept',13,10,'$'
msg_bad     db  '  LOST',13,10,'$'
msg_bye     db  13,10,'exiting - the DOS that answers this 4Ch unparks.',13,10,'$'

_PW     ENDS
        END     start
