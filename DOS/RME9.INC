;=====================================================================
; RME9.INC - every INT 21h RMDOS serves, as a line of text on the
;            Bochs debug port.  Build target "rme9".
;
; THE DISK TRACE (RMDBG.INC) STOPS WHAT IT WATCHES.  A BIOS disk write
; from inside a DOS call costs a Windows start-up about a hundred calls
; and then holds it on the splash screen for good, on 86Box and on
; DOSBox-X alike, so it cannot say where a release build stops.  This
; costs one OUT per character: DOSBox-X with "bochs debug port e9=true"
; puts what arrives on port E9h in its log, and a machine with nothing
; on the port ignores it.  Nothing is buffered, nothing is flushed, and
; there is nothing to lose when the machine never comes back.
;
; One line per call, written on the way in and finished on the way out,
; so a call that never returns is the line with no answer on it:
;
;   21 AX=3D00 BX=0000 CX=0000 DX=1D60 @1EAE:1DA6 [C:\WINDOWS\WIN.INI] -> 0005
;   21 AX=3D00 ... -> 0002 CY
;
; Included only when RMDOS_E9 is defined.
;=====================================================================

e9_i21in:
        pusha
        push    es
        mov     si,OFFSET e9_m21
        call    e9_str
        mov     ax,[bp+F_AX]
        call    e9_hex
        mov     si,OFFSET e9_mbx
        call    e9_str
        mov     ax,[bp+F_BX]
        call    e9_hex
        mov     si,OFFSET e9_mcx
        call    e9_str
        mov     ax,[bp+F_CX]
        call    e9_hex
        mov     si,OFFSET e9_mdx
        call    e9_str
        mov     ax,[bp+F_DX]
        call    e9_hex
        mov     si,OFFSET e9_mds
        call    e9_str
        mov     ax,[bp+F_DS]
        call    e9_hex
        mov     si,OFFSET e9_mss
        call    e9_str
        mov     ax,[bp+F_OWNSEG]        ; the caller's stack, where the
        call    e9_hex                  ; frame still is
        mov     al,':'
        call    e9_putc
        mov     ax,[bp+F_OWNOFF]
        call    e9_hex
        mov     si,OFFSET e9_mdep
        call    e9_str
        mov     al,byte ptr cs:[rd_stkdep]
        call    e9_hex2
        mov     al,'/'
        call    e9_putc
        mov     al,byte ptr cs:[rd_indos]
        call    e9_hex2
        mov     si,OFFSET e9_mat
        call    e9_str
        mov     ax,[bp+F_CS]
        call    e9_hex
        mov     al,':'
        call    e9_putc
        mov     ax,[bp+F_IP]
        call    e9_hex
        call    e9_sumchk               ; the code, still the code?

        ; what a program PRINTS, so a test run on RMDOS - which has no
        ; console anybody can read headless - reports into the log
        mov     ah,byte ptr [bp+F_AX+1]
        cmp     ah,02h
        je      e9i_02
        cmp     ah,09h
        je      e9i_09
        cmp     ah,40h
        je      e9i_40

        ; the name, for the calls that take one
        mov     di,OFFSET e9_named
e9i_scan:
        mov     al,cs:[di]
        cmp     al,0FFh
        je      e9i_done
        cmp     al,ah
        je      e9i_name
        inc     di
        jmp     short e9i_scan
e9i_name:
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        cmp     ah,6Ch
        jne     e9i_open
        mov     di,[bp+F_SI]            ; extended open: DS:SI
e9i_open:
        mov     al,' '
        call    e9_putc
        mov     al,'['
        call    e9_putc
        mov     cx,64
e9i_char:
        mov     al,es:[di]
        or      al,al
        jz      e9i_close
        cmp     al,' '
        jb      e9i_close
        call    e9_putc
        inc     di
        loop    e9i_char
e9i_close:
        mov     al,']'
        call    e9_putc
e9i_done:
        pop     es
        popa
        ret

e9i_02:
        mov     si,OFFSET e9_mtext
        call    e9_str
        mov     al,byte ptr [bp+F_DX]
        call    e9_vis
        jmp     short e9i_close
e9i_09:
        mov     si,OFFSET e9_mtext
        call    e9_str
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        mov     cx,160
e9i_09c:
        mov     al,es:[di]
        cmp     al,'$'
        je      e9i_close
        call    e9_vis
        inc     di
        loop    e9i_09c
        jmp     short e9i_close
e9i_40:
        mov     si,OFFSET e9_mtext
        call    e9_str
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        mov     cx,[bp+F_CX]
        cmp     cx,160
        jbe     e9i_40c
        mov     cx,160
e9i_40c:
        jcxz    e9i_close
e9i_40l:
        mov     al,es:[di]
        call    e9_vis
        inc     di
        loop    e9i_40l
        jmp     short e9i_close

; e9_vis - AL to the port, a control character as a visible escape
e9_vis:
        cmp     al,' '
        jae     e9v_out
        push    ax
        mov     al,'\'
        call    e9_putc
        pop     ax
        cmp     al,13
        jne     e9v_lf
        mov     al,'r'
        jmp     short e9v_out
e9v_lf:
        cmp     al,10
        jne     e9v_dot
        mov     al,'n'
        jmp     short e9v_out
e9v_dot:
        mov     al,'.'
e9v_out:
        call    e9_putc
        ret

e9_i21out:
        pusha
        mov     si,OFFSET e9_mret
        call    e9_str
        mov     ax,[bp+F_AX]
        call    e9_hex
        test    word ptr [bp+F_FL],FL_CF
        jz      e9o_nl
        mov     si,OFFSET e9_mcy
        call    e9_str
e9o_nl:
        mov     al,10
        call    e9_putc
        popa
        ret

;---------------------------------------------------------------------
; e9_hook6 / e9_i6 - INT 6, the invalid opcode, as a line in the log.
;
; A real-mode program that jumps into memory that is not there runs
; FF FF - which is no instruction - and the BIOS's INT 6 IRETs straight
; back onto it, forever, with nothing to say where it came from.  This
; one prints the address, the bytes there, and the eight words above
; the IRET frame - where a stray RET or IRET found its way - and then
; stops the machine with interrupts off, so the log ends on the answer
; instead of on a million copies of the question.
;---------------------------------------------------------------------
e9_hook6:
        push    ax
        push    es
        xor     ax,ax
        mov     es,ax
        mov     ax,word ptr es:[6*4]
        mov     word ptr cs:[e9_old6],ax
        mov     ax,word ptr es:[6*4+2]
        mov     word ptr cs:[e9_old6+2],ax
        mov     word ptr es:[6*4],OFFSET e9_i6
        mov     word ptr es:[6*4+2],cs
        pop     es
        pop     ax
        ret

; e9_unhook6 - and the vector back, before this image is given away:
; a hook left pointing into freed memory is where the box went after
; the first enhanced-mode park came home.
e9_unhook6:
        push    ax
        push    es
        xor     ax,ax
        mov     es,ax
        mov     ax,word ptr cs:[e9_old6]
        mov     word ptr es:[6*4],ax
        mov     ax,word ptr cs:[e9_old6+2]
        mov     word ptr es:[6*4+2],ax
        pop     es
        pop     ax
        ret

e9_old6     dd  0

e9_i6:
        push    bp
        mov     bp,sp
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        mov     si,OFFSET e9_mud
        call    e9_str
        mov     ax,[bp+4]               ; CS
        call    e9_hex
        mov     al,':'
        call    e9_putc
        mov     ax,[bp+2]               ; IP
        call    e9_hex
        mov     si,OFFSET e9_mbytes
        call    e9_str
        mov     es,[bp+4]
        mov     di,[bp+2]
        mov     cx,8
e9u_bytes:
        mov     al,es:[di]
        push    cx
        mov     ah,al
        mov     bl,ah
        shr     al,4
        call    e9u_nib
        mov     al,bl
        call    e9u_nib
        mov     al,' '
        call    e9_putc
        pop     cx
        inc     di
        loop    e9u_bytes
        mov     si,OFFSET e9_mstack
        call    e9_str
        mov     ax,ss
        call    e9_hex
        mov     al,':'
        call    e9_putc
        lea     ax,[bp+8]
        call    e9_hex
        mov     al,' '
        call    e9_putc
        mov     di,8
e9u_stack:
        mov     ax,[bp+di]
        push    di
        call    e9_hex
        mov     al,' '
        call    e9_putc
        pop     di
        add     di,2
        cmp     di,8 + 16
        jb      e9u_stack
        mov     al,10
        call    e9_putc
        ; Spin, with interrupts ON.  CLI and HLT would be the natural
        ; stop, and DOSBox-X treats a halted CPU with nothing to wake it
        ; as a reason to stop checking its own -time-limit: the run
        ; never ends, the process has to be killed, and the log - this
        ; line with it - is lost in its buffers.
        sti
e9u_stop:
        jmp     short e9u_stop

e9u_nib:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9n_out
        add     al,7
e9n_out:
        call    e9_putc
        ret

e9_mud      db  '#UD at ',0
e9_mbytes   db  ' bytes ',0
e9_mstack   db  ' stack ',0

; e9_str - CS:SI, NUL-terminated, to the port.  AL and SI are used.
e9_str:
        mov     al,cs:[si]
        or      al,al
        jz      e9s_out
        call    e9_putc
        inc     si
        jmp     short e9_str
e9s_out:
        ret

; e9_hex - AX as four hex digits.  AX is used; BX and CX come back,
; because e9_xin and e9_xout print BX and DX AFTER AX through this and
; the first version printed AX three times over.
e9_hex:
        push    bx
        push    cx
        mov     bx,ax
        mov     cx,4
e9h_dig:
        rol     bx,4
        mov     al,bl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9h_out
        add     al,7
e9h_out:
        call    e9_putc
        loop    e9h_dig
        pop     cx
        pop     bx
        ret

e9_m21      db  '21 AX=',0
e9_mbx      db  ' BX=',0
e9_mcx      db  ' CX=',0
e9_mdx      db  ' DX=',0
e9_mds      db  ' ds=',0
e9_mss      db  ' ss=',0
e9_mdep     db  ' d=',0
e9_mat      db  ' @',0
e9_mret     db  ' -> ',0
e9_mcy      db  ' CY',0
e9_mtext    db  ' [',0
; the calls whose DS:DX (DS:SI for 6Ch) is a name worth printing
e9_named    db  39h, 3Ah, 3Bh, 3Ch, 3Dh, 41h, 43h, 4Bh, 4Eh, 56h, 5Ah, 5Bh, 6Ch, 0FFh

;---------------------------------------------------------------------
; e9_xin / e9_xout - the XMS entry, as the same kind of line:
;
;   XM AH=0B DX=0000 BX=0000 CX=0000 len=00008000 src=0000:35300000 dst=0001:00000000 -> AX=0001 BX=0000 DX=0000
;
; Called from rx_entry with the caller's registers still in place (DS
; and ES are already ours), and from rx_ret with the answer in them.
; A 0Bh prints its descriptor, out of the copy rx_entry just made.
;---------------------------------------------------------------------
e9_xin:
        pusha
        push    ax
        mov     si,OFFSET e9_mxm
        call    e9_str
        pop     ax
        push    ax
        mov     al,ah
        xor     ah,ah
        call    e9_hex2
        mov     si,OFFSET e9_mdx
        call    e9_str
        mov     ax,dx
        call    e9_hex
        mov     si,OFFSET e9_mbx
        call    e9_str
        mov     ax,bx
        call    e9_hex
        mov     si,OFFSET e9_mcx
        call    e9_str
        mov     ax,cx
        call    e9_hex
        pop     ax
        cmp     ah,0Bh
        jne     e9x_in
        mov     si,OFFSET e9_mlen
        call    e9_str
        mov     ax,word ptr [rx_desc + XM_LEN + 2]
        call    e9_hex
        mov     ax,word ptr [rx_desc + XM_LEN]
        call    e9_hex
        mov     si,OFFSET e9_msrc
        call    e9_str
        mov     ax,word ptr [rx_desc + XM_SRCH]
        call    e9_hex
        mov     al,':'
        call    e9_putc
        mov     ax,word ptr [rx_desc + XM_SRCO + 2]
        call    e9_hex
        mov     ax,word ptr [rx_desc + XM_SRCO]
        call    e9_hex
        mov     si,OFFSET e9_mdst
        call    e9_str
        mov     ax,word ptr [rx_desc + XM_DSTH]
        call    e9_hex
        mov     al,':'
        call    e9_putc
        mov     ax,word ptr [rx_desc + XM_DSTO + 2]
        call    e9_hex
        mov     ax,word ptr [rx_desc + XM_DSTO]
        call    e9_hex
e9x_in:
        popa
        ret

e9_xout:
        pusha
        push    ax
        mov     si,OFFSET e9_mxret
        call    e9_str
        pop     ax
        call    e9_hex
        mov     si,OFFSET e9_mbx
        call    e9_str
        mov     ax,bx
        call    e9_hex
        mov     si,OFFSET e9_mdx
        call    e9_str
        mov     ax,dx
        call    e9_hex
        mov     al,10
        call    e9_putc
        popa
        ret

; e9_hex2 - AL as two hex digits.  AX, BX and CX are used.
e9_hex2:
        mov     bl,al
        mov     cx,2
        rol     bl,4
e9h2_dig:
        mov     al,bl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9h2_out
        add     al,7
e9h2_out:
        call    e9_putc
        rol     bl,4
        loop    e9h2_dig
        ret

;---------------------------------------------------------------------
; e9_i2f / e9_i15 - a software interrupt RMDOS answers directly, with
; the registers as they arrived.  One line, no answer: these handlers
; return through several IRETs and the question is what was asked.
;---------------------------------------------------------------------
e9_i2f:
        pusha
        push    ds
        push    cs
        pop     ds
        push    ax
        mov     si,OFFSET e9_m2f
        call    e9_str
        pop     ax
        push    ax
        call    e9_regs
        pop     ax
        ; 1607h's device-size question (CX=5) names the driver in ES:DI,
        ; and so does the rest of DOSMGR's callout - a second line
        cmp     ax,1607h
        jne     e9f_done
        mov     si,OFFSET e9_mesdi
        call    e9_str
        mov     ax,es
        call    e9_hex
        mov     al,':'
        call    e9_putc
        mov     ax,di
        call    e9_hex
        mov     al,10
        call    e9_putc
e9f_done:
        pop     ds
        popa
        ret
e9_mesdi    db  '   ES:DI=',0

e9_i15:
        pusha
        push    ds
        push    cs
        pop     ds
        push    ax
        mov     si,OFFSET e9_m15
        call    e9_str
        pop     ax
        call    e9_regs
        pop     ds
        popa
        ret

; e9_regs - AX, BX, CX, DX as arrived (AX in AX, the rest untouched)
e9_regs:
        push    dx
        push    cx
        push    bx
        call    e9_hex
        mov     si,OFFSET e9_mbx
        call    e9_str
        pop     ax
        call    e9_hex
        mov     si,OFFSET e9_mcx
        call    e9_str
        pop     ax
        call    e9_hex
        mov     si,OFFSET e9_mdx
        call    e9_str
        pop     ax
        call    e9_hex
        mov     al,10
        call    e9_putc
        ret

e9_mxm      db  'XM AH=',0
e9_mxret    db  ' -> AX=',0
e9_mlen     db  ' len=',0
e9_msrc     db  ' src=',0
e9_mdst     db  ' dst=',0
e9_m2f      db  '2F AX=',0
e9_m15      db  '15 AX=',0

;--- e9_a20init - rx_init's verdict on the gate: the method it found
;--- (0 none, 1 keyboard controller, 2 port 92h) and the state after
;--- its attempt to shut it (1 open, 0 shut).
e9_a20init:
        pusha
        mov     si,OFFSET e9_ma20
        call    e9_str
        mov     al,byte ptr cs:[rx_a20meth]
        xor     ah,ah
        call    e9_hex
        mov     si,OFFSET e9_ma20s
        call    e9_str
        call    rx_a20test
        call    e9_hex
        mov     al,10
        call    e9_putc
        popa
        ret

e9_ma20     db  'A20 init: method ',0
e9_ma20s    db  ' state after shutting ',0

;--- e9_env - exec_psp has just copied the child's environment: AX = the
;--- new block (0 = none).  The source, the new block, the lengths, the
;--- child's PSP, and then the path that follows the strings in the new
;--- block - which is the thing KRNL386 opens to find itself.
e9_env:
        pusha
        push    ds
        push    es
        push    ax
        mov     si,OFFSET e9_menv
        call    e9_str
        mov     ax,word ptr cs:[ep_envsrc]
        call    e9_hex
        mov     si,OFFSET e9_menvn
        call    e9_str
        pop     ax
        push    ax
        call    e9_hex
        mov     si,OFFSET e9_menvl
        call    e9_str
        mov     ax,word ptr cs:[ep_envlen]
        call    e9_hex
        mov     al,'/'
        call    e9_putc
        mov     ax,word ptr cs:[ep_envnam]
        call    e9_hex
        mov     si,OFFSET e9_menvp
        call    e9_str
        mov     ax,word ptr cs:[rd_execpsp]
        call    e9_hex
        pop     ax
        or      ax,ax
        jz      e9e_done
        mov     si,OFFSET e9_menvt
        call    e9_str
        mov     es,ax
        xor     di,di
        mov     cx,8000h
e9e_scan:                               ; the empty string that ends them
        cmp     byte ptr es:[di],0
        je      e9e_end
e9e_ch:
        inc     di
        dec     cx
        jz      e9e_done
        cmp     byte ptr es:[di],0
        jne     e9e_ch
        inc     di
        dec     cx
        jnz     e9e_scan
        jmp     short e9e_done
e9e_end:
        add     di,3                    ; past it and the count word
        mov     cx,70
e9e_path:
        mov     al,es:[di]
        or      al,al
        jz      e9e_done
        call    e9_vis
        inc     di
        loop    e9e_path
e9e_done:
        mov     al,10
        call    e9_putc
        pop     es
        pop     ds
        popa
        ret

e9_menv     db  'ENV src=',0
e9_menvn    db  ' new=',0
e9_menvl    db  ' len=',0
e9_menvp    db  ' psp=',0
e9_menvt    db  ' path: ',0

;--- e9_disk / e9_diskend - every BIOS transfer rd_rdsec/rd_wrsec makes:
;--- "DK r 80 lba=XXXXXXXX n=XXXX" on the way in, " +" or " -" on the way
;--- out, so that a read that never comes back is the line with no end.
e9_disk:
        pushad
        push    ds
        push    cs
        pop     ds
        push    eax
        push    cx
        mov     si,OFFSET e9_mdk
        call    e9_str
        mov     al,'r'
        cmp     byte ptr [rd_rw],0
        je      e9d_rw
        mov     al,'w'
e9d_rw:
        call    e9_putc
        mov     al,' '
        call    e9_putc
        mov     al,[rd_bios]
        call    e9_hex2
        mov     si,OFFSET e9_mdkl
        call    e9_str
        pop     cx
        pop     eax
        push    eax
        shr     eax,16
        call    e9_hex
        pop     eax
        call    e9_hex
        mov     si,OFFSET e9_mdkn
        call    e9_str
        mov     ax,cx
        call    e9_hex
        mov     si,OFFSET e9_mdka       ; " at ES:BX", the buffer - a
        call    e9_str                  ; floppy transfer that straddles
        mov     ax,es                   ; a 64K page is the one the BIOS
        call    e9_hex                  ; refuses
        mov     al,':'
        call    e9_putc
        mov     ax,bx
        call    e9_hex
        pop     ds
        popad
        ret

;--- e9_diskerr - " e=XX" after a BIOS transfer that failed: AH as the
;--- BIOS left it (rd_raw's rdr_err, before the reset and the retry).
e9_diskerr:
        pushad
        push    ds
        push    cs
        pop     ds
        mov     [e9_dbx],bx             ; kept first: the printers use them
        mov     [e9_dcx],cx
        mov     [e9_ddx],dx
        mov     [e9_des],es
        mov     si,OFFSET e9_mdke
        call    e9_str
        mov     al,ah
        call    e9_hex2
        mov     al,' '                  ; and the buffer and CHS the BIOS
        call    e9_putc                 ; was given, which it leaves alone
        mov     ax,[e9_des]
        call    e9_hex
        mov     al,':'
        call    e9_putc
        mov     ax,[e9_dbx]
        call    e9_hex
        mov     al,'/'
        call    e9_putc
        mov     ax,[e9_dcx]
        call    e9_hex
        mov     al,'/'
        call    e9_putc
        mov     ax,[e9_ddx]
        call    e9_hex
        pop     ds
        popad
        ret
e9_dbx      dw  0
e9_dcx      dw  0
e9_ddx      dw  0
e9_des      dw  0

e9_diskend:
        push    ax
        mov     al,' '
        call    e9_putc
        pop     ax
        call    e9_putc
        mov     al,10
        call    e9_putc
        ret

e9_mdk      db  'DK ',0
e9_mdkl     db  ' lba=',0
e9_mdkn     db  ' n=',0
e9_mdka     db  ' at ',0
e9_mdke     db  ' e=',0

;--- e9_sumchk - a sixteen-bit sum of the bytes from rd_name83 to the
;--- end of rd_path, printed whenever it differs from last time: the
;--- call on whose line it changes is the call that overwrote the code.
e9_sumchk:
        push    ax
        push    bx
        push    cx
        push    si
        mov     si,OFFSET rd_name83
        mov     cx,OFFSET rp_bad
        sub     cx,si
        xor     bx,bx
e9s_add:
        mov     al,cs:[si]
        xor     ah,ah
        add     bx,ax
        inc     si
        loop    e9s_add
        cmp     bx,word ptr cs:[e9_lastsum]
        je      e9s_same
        mov     ax,bx
        mov     bx,word ptr cs:[e9_lastsum]
        mov     word ptr cs:[e9_lastsum],ax
        push    bx
        mov     si,OFFSET e9_msum
        call    e9_str
        call    e9_hex
        mov     al,'('
        call    e9_putc
        pop     ax
        call    e9_hex
        mov     al,')'
        call    e9_putc
e9s_same:
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

e9_lastsum  dw  0
e9_msum     db  ' SUM=',0

;--- e9_mcbs - RMDOS's own segment, then the MCB chain as handed over:
;--- one line per block - segment, owner, size in paragraphs, name.
e9_mcbs:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        mov     si,OFFSET e9_mrm
        call    e9_str
        mov     ax,cs
        call    e9_hex
        mov     al,10
        call    e9_putc
        mov     bx,[h_mcbhead]
e9m_one:
        mov     es,bx
        mov     si,OFFSET e9_mmcb
        call    e9_str
        mov     ax,bx
        call    e9_hex
        mov     al,' '
        call    e9_putc
        mov     al,es:[0]
        call    e9_putc                 ; 'M' or 'Z'
        mov     al,' '
        call    e9_putc
        mov     ax,es:[1]
        call    e9_hex
        mov     al,' '
        call    e9_putc
        mov     ax,es:[3]
        call    e9_hex
        mov     al,' '
        call    e9_putc
        mov     cx,8
        mov     di,8
e9m_name:
        mov     al,es:[di]
        call    e9_vis
        inc     di
        loop    e9m_name
        mov     al,10
        call    e9_putc
        cmp     byte ptr es:[0],'Z'
        je      e9m_done
        cmp     byte ptr es:[0],'M'
        jne     e9m_done
        mov     ax,es:[3]
        add     bx,ax
        inc     bx
        cmp     bx,0A000h
        jb      e9m_one
e9m_done:
        call    e9_ports                ; and the machine state Windows gets
        call    e9_devs                 ; and the device chain it walks
        pop     es
        pop     ds
        popa
        ret

;=====================================================================
; e9_prehook - IN FRONT OF A DRIVER'S HOOK, so the trace sees what the
; driver answers itself.  A driver CONFIG.SYS loaded may hook INT 21h
; or 2Fh at its INIT (IFSHLP.SYS hooks both), and what it answers
; without passing on never reaches this DOS or its trace.  When 21h or
; 2Fh points anywhere but here, a logging stub goes in front of it:
; "p21 AX=" or "p2F AX=" and BX, then on to whoever was there.  The
; kernel takes these vectors back on the way home - they point into
; this image (PARK.INC, park_restvecs) - so nothing is undone here.
;=====================================================================
e9_prehook:
        pusha
        push    es
        xor     ax,ax
        mov     es,ax
        mov     ax,cs
        cmp     word ptr es:[21h*4+2],ax
        je      e9ph_2f
        mov     ax,es:[21h*4]
        mov     word ptr [e9p21_old],ax
        mov     ax,es:[21h*4+2]
        mov     word ptr [e9p21_old+2],ax
        cli
        mov     word ptr es:[21h*4],OFFSET e9p21
        mov     word ptr es:[21h*4+2],cs
        sti
e9ph_2f:
        mov     ax,cs
        cmp     word ptr es:[2Fh*4+2],ax
        je      e9ph_out
        mov     ax,es:[2Fh*4]
        mov     word ptr [e9p2f_old],ax
        mov     ax,es:[2Fh*4+2]
        mov     word ptr [e9p2f_old+2],ax
        cli
        mov     word ptr es:[2Fh*4],OFFSET e9p2f
        mov     word ptr es:[2Fh*4+2],cs
        sti
e9ph_out:
        pop     es
        popa
        ret

e9p21:
        push    si
        mov     si,OFFSET e9_mp21
        call    e9_prelog
        pop     si
        jmp     dword ptr cs:[e9p21_old]
e9p2f:
        push    si
        mov     si,OFFSET e9_mp2f
        call    e9_prelog
        pop     si
        jmp     dword ptr cs:[e9p2f_old]

;--- e9_prelog - CS:SI the label; AX and BX are the call's.  Everything
;--- kept, the flags too.
e9_prelog:
        pushf
        pusha
        push    ds
        push    cs
        pop     ds
        push    bx
        push    ax
        call    e9_str
        pop     ax
        call    e9_hex
        mov     si,OFFSET e9_mpbx
        call    e9_str
        pop     ax
        call    e9_hex
        mov     al,10
        call    e9_putc
        pop     ds
        popa
        popf
        ret

e9p21_old   dd  0
e9p2f_old   dd  0
e9_mp21     db  'p21 AX=',0
e9_mp2f     db  'p2F AX=',0
e9_mpbx     db  ' BX=',0

;=====================================================================
; e9_i21pre - "21> AX=xxxx", the moment a call is on RMDOS's stack and
; before the critical section DOSMGR keeps round it (crit_in).  The
; full line e9_i21in writes comes after that section is ours, so a
; call that waits for it - a VxD holding it while it asks DOS
; something - used to leave no trace at all (2026-10-06).
;=====================================================================
e9_i21pre:
        pusha
        push    ds
        push    cs
        pop     ds
        mov     si,OFFSET e9_mpre
        call    e9_str
        mov     ax,[bp+F_AX]
        call    e9_hex
        mov     al,10
        call    e9_putc
        pop     ds
        popa
        ret
e9_mpre     db  '21> AX=',0

;=====================================================================
; e9_devs - the device chain as RMDOS publishes it, a line a header:
; "DEV ssss:oooo attr NAME".  The drivers CONFIG.SYS loaded are the
; lines between NUL and CON (rd_chain, RMINFO.INC).
;=====================================================================
e9_devs:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        mov     es,[rd_lowseg]
        mov     bx,RL_NULDEV
        mov     cx,RD_CHAINMAX + 4
e9d_one:
        mov     si,OFFSET e9_mdev
        call    e9_str
        mov     ax,es
        call    e9_hex
        mov     al,':'
        call    e9_putc
        mov     ax,bx
        call    e9_hex
        mov     al,' '
        call    e9_putc
        mov     ax,es:[bx+4]
        call    e9_hex
        mov     al,' '
        call    e9_putc
        push    cx
        mov     cx,8
        lea     di,[bx+0Ah]
e9d_name:
        mov     al,es:[di]
        call    e9_vis
        inc     di
        loop    e9d_name
        pop     cx
        mov     al,10
        call    e9_putc
        cmp     word ptr es:[bx],0FFFFh
        je      e9d_done
        les     bx,dword ptr es:[bx]
        loop    e9d_one
e9d_done:
        pop     es
        pop     ds
        popa
        ret
e9_mdev     db  'DEV ',0

;=====================================================================
; e9_ports - the printer and interrupt state RMDOS is born with, one
; line: the BIOS's printer port table (40:08) and time-outs (40:78),
; the three registers of the port at 378h, both PIC masks and the
; edge/level registers.  Added 2026-10-06 for WfW 3.11's VPD, which
; stops on RMDOS when an LPT port exists and not on a real DOS on the
; same machine - so the two machines are to be compared here.
;=====================================================================
e9_ports:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        xor     ax,ax
        mov     es,ax                   ; ES:[4xxh] = the BIOS data area
        mov     si,OFFSET e9_mbda
        call    e9_str
        mov     bx,408h
        mov     cx,4
e9p_lpt:
        mov     ax,es:[bx]
        call    e9_hex
        mov     al,' '
        call    e9_putc
        add     bx,2
        loop    e9p_lpt
        mov     si,OFFSET e9_mto
        call    e9_str
        mov     bx,478h
        mov     cx,4
e9p_to:
        mov     al,es:[bx]
        push    bx                      ; e9_hex2 uses BL and CX
        push    cx
        call    e9_hex2
        mov     al,' '
        call    e9_putc
        pop     cx
        pop     bx
        inc     bx
        loop    e9p_to
        mov     si,OFFSET e9_mlpt
        call    e9_str
        mov     dx,378h
        mov     cx,3
e9p_reg:
        in      al,dx
        push    cx
        call    e9_hex2
        mov     al,' '
        call    e9_putc
        pop     cx
        inc     dx
        loop    e9p_reg
        mov     si,OFFSET e9_mpic
        call    e9_str
        in      al,21h
        call    e9_hex2
        mov     al,' '
        call    e9_putc
        in      al,0A1h
        call    e9_hex2
        mov     si,OFFSET e9_melcr
        call    e9_str
        mov     dx,4D0h
        in      al,dx
        call    e9_hex2
        mov     al,' '
        call    e9_putc
        inc     dx
        in      al,dx
        call    e9_hex2
        mov     al,10
        call    e9_putc

        ; ...and the whole of 0:0 to 0:4FF, the vector table and the
        ; BIOS data area, sixteen bytes a line, so that a real DOS's
        ; copy (PICPROBE's C:\LOWMEM.BIN) can be compared byte for byte.
        xor     bx,bx
e9p_row:
        mov     si,OFFSET e9_mlow
        call    e9_str
        mov     ax,bx
        push    bx
        call    e9_hex
        pop     bx
        mov     cx,16
e9p_byte:
        mov     al,' '
        call    e9_putc
        mov     al,es:[bx]
        push    bx
        push    cx
        call    e9_hex2
        pop     cx
        pop     bx
        inc     bx
        loop    e9p_byte
        mov     al,10
        call    e9_putc
        cmp     bx,500h
        jb      e9p_row
        pop     es
        pop     ds
        popa
        ret

e9_mlow     db  'LOW ',0
e9_mbda     db  'BDA 40:08 ',0
e9_mto      db  ' 40:78 ',0
e9_mlpt     db  ' LPT 378 ',0
e9_mpic     db  ' PIC 21/A1 ',0
e9_melcr    db  ' ELCR ',0

e9_mrm      db  'RMDOS at ',0
e9_mmcb     db  'MCB ',0

;=====================================================================
; e9_putc - one character of the trace, AL.  Every line above comes
; through here, and it goes to port E9h always.  With RMDOS_E9LPT
; (build target "rme9lpt") the same character also goes out of LPT1,
; a byte on the data lines and a pulse on STROBE, so that the trace
; can be read on a machine with nothing on port E9h: 86Box's "Generic
; Text Printer" (lpt1_device = text_prt in 86box.cfg) keeps whatever
; arrives on the port as a text file in the VM's printer folder, and
; writes the file out when the machine is closed - which the rig does
; with WM_CLOSE, so a Windows that never came back still leaves its
; last lines there.  This is the trace for 386 enhanced mode, which
; DOSBox-X cannot run and 86Box cannot log.
;
; BUSY is waited for with a count, not for ever: a port with nothing
; on it reads FFh, which says "not busy" at once, and one that stays
; busy is given up on after the count rather than holding the DOS.
; Nothing is kept but AL's value and every other register and the
; flags are put back, because the callers were written against an
; OUT, which changes none of them.
;=====================================================================
e9_putc:
        out     0E9h,al
IFDEF RMDOS_E9LPT
        pushf
        push    dx
        push    cx
        push    ax
        mov     dx,379h                 ; status: bit 7 set = not busy
        mov     cx,4000h
e9p_wait:
        in      al,dx
        test    al,80h
        loopz   e9p_wait
        pop     ax
        push    ax
        mov     dx,378h                 ; the byte on the data lines
        out     dx,al
        mov     dx,37Ah
        mov     al,0Dh                  ; SELECT, INIT high, STROBE on
        out     dx,al
        mov     al,0Ch                  ; ...and off: the printer latched it
        out     dx,al
        pop     ax
        pop     cx
        pop     dx
        popf
ENDIF
IFDEF RMDOS_E9COM
        ; ...OR OUT OF COM1, 115200 8N1, when the parallel port of the
        ; machine cannot be trusted: on 86Box's p3bf the text printer
        ; saw the control byte on the data lines whatever order the two
        ; were written in (2026-10-06), where the UART is just a UART.
        ; 86Box with serial1_passthrough_enabled=1 and a "Serial
        ; Passthrough Device #1" in mode 0 serves the port as a named
        ; pipe the host reads.  The UART is set up on the first
        ; character and THRE is waited for with a count, as above.
        pushf
        push    dx
        push    cx
        push    ax
        cmp     byte ptr cs:[e9_comon],0
        jne     e9c_ready
        mov     byte ptr cs:[e9_comon],1
        mov     dx,3FBh                 ; LCR: DLAB on
        mov     al,80h
        out     dx,al
        mov     dx,3F8h                 ; divisor 1 = 115200
        mov     al,1
        out     dx,al
        inc     dx
        xor     al,al
        out     dx,al
        mov     dx,3FBh                 ; 8N1, DLAB off
        mov     al,03h
        out     dx,al
        mov     dx,3F9h                 ; no interrupts
        xor     al,al
        out     dx,al
        mov     dx,3FAh                 ; no FIFO
        out     dx,al
        mov     dx,3FCh                 ; DTR, RTS
        mov     al,03h
        out     dx,al
e9c_ready:
        mov     dx,3FDh                 ; LSR: bit 5 = holding register empty
        mov     cx,4000h
e9c_wait:
        in      al,dx
        test    al,20h
        loopz   e9c_wait
        pop     ax
        push    ax
        mov     dx,3F8h
        out     dx,al
        pop     ax
        pop     cx
        pop     dx
        popf
ENDIF
        ret
IFDEF RMDOS_E9COM
e9_comon    db  0
ENDIF
