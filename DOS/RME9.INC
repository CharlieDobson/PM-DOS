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
        mov     si,OFFSET e9_mat
        call    e9_str
        mov     ax,[bp+F_CS]
        call    e9_hex
        mov     al,':'
        out     0E9h,al
        mov     ax,[bp+F_IP]
        call    e9_hex

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
        out     0E9h,al
        mov     al,'['
        out     0E9h,al
        mov     cx,64
e9i_char:
        mov     al,es:[di]
        or      al,al
        jz      e9i_close
        cmp     al,' '
        jb      e9i_close
        out     0E9h,al
        inc     di
        loop    e9i_char
e9i_close:
        mov     al,']'
        out     0E9h,al
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
        out     0E9h,al
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
        out     0E9h,al
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
        out     0E9h,al
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
        mov     word ptr es:[6*4],OFFSET e9_i6
        mov     word ptr es:[6*4+2],cs
        pop     es
        pop     ax
        ret

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
        out     0E9h,al
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
        out     0E9h,al
        pop     cx
        inc     di
        loop    e9u_bytes
        mov     si,OFFSET e9_mstack
        call    e9_str
        mov     ax,ss
        call    e9_hex
        mov     al,':'
        out     0E9h,al
        lea     ax,[bp+8]
        call    e9_hex
        mov     al,' '
        out     0E9h,al
        mov     di,8
e9u_stack:
        mov     ax,[bp+di]
        push    di
        call    e9_hex
        mov     al,' '
        out     0E9h,al
        pop     di
        add     di,2
        cmp     di,8 + 16
        jb      e9u_stack
        mov     al,10
        out     0E9h,al
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
        out     0E9h,al
        ret

e9_mud      db  '#UD at ',0
e9_mbytes   db  ' bytes ',0
e9_mstack   db  ' stack ',0

; e9_str - CS:SI, NUL-terminated, to the port.  AL and SI are used.
e9_str:
        mov     al,cs:[si]
        or      al,al
        jz      e9s_out
        out     0E9h,al
        inc     si
        jmp     short e9_str
e9s_out:
        ret

; e9_hex - AX as four hex digits.  AX, BX and CX are used.
e9_hex:
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
        out     0E9h,al
        loop    e9h_dig
        ret

e9_m21      db  '21 AX=',0
e9_mbx      db  ' BX=',0
e9_mcx      db  ' CX=',0
e9_mdx      db  ' DX=',0
e9_mat      db  ' @',0
e9_mret     db  ' -> ',0
e9_mcy      db  ' CY',0
e9_mtext    db  ' [',0
; the calls whose DS:DX (DS:SI for 6Ch) is a name worth printing
e9_named    db  39h, 3Ah, 3Bh, 3Ch, 3Dh, 41h, 43h, 4Bh, 4Eh, 56h, 5Ah, 5Bh, 6Ch, 0FFh
