;=====================================================================
; HOOKTEST.ASM - HOOKTEST.COM: a DOS program's own hooks on the DOS
;                vectors, and a ^C handed to its own INT 23h.
;
; Until v0.60 the V86 monitor trapped INT 20h-2Fh before they reached
; the guest's vector table, so a program that hooked INT 21h or 2Fh -
; which is what a TSR is - was never called, and a ^C ended the whole
; DOS box whatever the program's INT 23h handler would have said.  Now
; the table points at the DOS vector stubs (VDM.INC, vdm_stubs) and a
; ^C goes to the program the way DOS sends it.  This checks both, from
; inside a box, the way a program would find out:
;
;   INT 21h and 2Fh hooked here are called, and chain to the kernel
;   the stub's old vector can be CALLED, as a TSR chains to it
;   INT 29h writes to the screen (it was an IRET that lost the char)
;   INT 25h reads a sector, both forms, and leaves its flags pushed
;   INT 26h writes one back
;   a ^C handler that IRETs, or RETF 2s, or RETFs with the carry clear,
;     has the call made again; one that changes AH gets that function
;   one that RETFs with the carry set ends the program - only it - and
;     so does having no handler at all; 4Dh says ^C
;   a program that is its own parent is sent to its terminate address
;   INT 24h hooked by a child is put back when the child ends
;
; A ^C is made by putting Ctrl+C into the BIOS keyboard buffer with
; INT 16h AH=05h: the kernel's console reads the same buffer (IO\KEYBD
; .INC), so it arrives exactly as a key does.  Headless, with no key
; ever pressed.
;
; The cases that END a program run in a child - this program again,
; with a letter in its tail saying which case it is.  The exit code is
; the number of failures.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_HK     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_HK, DS:_HK, ES:_HK, SS:_HK
        ORG     0

STACK_BYTES     EQU     512
MPX_ID          EQU     0C1h            ; the multiplex id the hook answers
KEY_CTRLC       EQU     2E03h           ; scan code and ASCII of Ctrl+C

start:
;--- a letter in the tail: which child case this run is ---------------
        mov     si,81h
        mov     cl,byte ptr ds:[80h]
        xor     ch,ch
        jcxz    main_test
arg_scan:
        mov     al,[si]
        cmp     al,' '
        je      arg_skip
        cmp     al,'/'
        je      arg_skip
        cmp     al,'-'
        jne     arg_got
arg_skip:
        inc     si
        loop    arg_scan
        jmp     short main_test
arg_got:
        and     al,0DFh
        cmp     al,'K'
        je      child_kill
        cmp     al,'D'
        je      child_default
        cmp     al,'P'
        je      child_selfpar
        cmp     al,'V'
        je      child_vec24
        cmp     al,'S'
        je      stuff_shell
        cmp     al,'T'
        je      stuff_ccfor
        cmp     al,'I'
        je      answerer
        cmp     al,'U'
        je      stuff_cc1
        cmp     al,'C'
        je      stuff_ctrlc
        cmp     al,'H'
        je      stuff_ver
        cmp     al,'L'
        je      logger
        cmp     al,'B'
        je      show_keys
        cmp     al,'Q'
        je      show_vecs
        cmp     al,'W'
        je      show_screen

main_test:
;--- room for a child ---------------------------------------------------
        mov     bx,OFFSET prog_end
        add     bx,STACK_BYTES + 15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti
        call    find_self

        mov     dx,OFFSET m_hi
        call    puts

        mov     ax,3523h                ; ^C's vector as the box began,
        int     21h                     ; put back after every case
        mov     word ptr [orig23],bx
        mov     word ptr [orig23+2],es
        push    cs
        pop     es

        call    t_hook21
        call    t_hook2F
        call    t_chain
        call    t_int29
        call    t_int25
        call    t_int26
        call    t_cc_iret
        call    t_cc_retf2
        call    t_cc_retfnc
        call    t_cc_newah
        call    t_cc_kill
        call    t_cc_default
        call    t_selfpar
        call    t_vec24
        call    t_dos12
        call    t_status

        mov     dx,OFFSET m_done
        call    puts
        mov     ax,[nfail]
        call    putdec
        mov     dx,OFFSET m_fails
        call    puts
        mov     al,byte ptr [nfail]
        mov     ah,4Ch
        int     21h

;=====================================================================
; The hooks
;=====================================================================
old21   dd      0
old2F   dd      0
orig23  dd      0
n21     dw      0
n2F     dw      0
ncc     dw      0

hk21:
        cmp     ah,30h
        jne     hk21_on
        inc     word ptr cs:[n21]
hk21_on:
        jmp     dword ptr cs:[old21]

hk2F:
        inc     word ptr cs:[n2F]
        cmp     ah,MPX_ID
        jne     hk2F_on
        mov     al,0FFh
        mov     bx,'KH'
        iret
hk2F_on:
        jmp     dword ptr cs:[old2F]

;--- the four ^C handlers that let the call go on --------------------
cc_iret:
        inc     word ptr cs:[ncc]
        iret
cc_retf2:
        inc     word ptr cs:[ncc]
        retf    2
cc_retfnc:
        inc     word ptr cs:[ncc]
        clc
        retf
cc_newah:
        inc     word ptr cs:[ncc]
        mov     ah,30h                  ; and the call made again is 30h
        iret

;=====================================================================
; The cases
;=====================================================================

;--- INT 21h through a hook ---------------------------------------------
t_hook21:
        mov     ax,3521h
        int     21h
        mov     word ptr [old21],bx
        mov     word ptr [old21+2],es
        push    cs
        pop     es
        mov     dx,OFFSET hk21
        mov     ax,2521h
        int     21h
        mov     word ptr [n21],0
        mov     ah,30h
        int     21h
        mov     [t_ax],ax
        push    ds
        lds     dx,[old21]
        mov     ax,2521h
        int     21h
        pop     ds
        cmp     word ptr [n21],1
        jne     th21_bad
        cmp     byte ptr [t_ax],0
        je      th21_bad
        mov     dx,OFFSET m_h21
        jmp     pass
th21_bad:
        mov     dx,OFFSET m_h21
        jmp     fail

;--- INT 2Fh through a hook, and on to the kernel ----------------------
t_hook2F:
        mov     ax,352Fh
        int     21h
        mov     word ptr [old2F],bx
        mov     word ptr [old2F+2],es
        push    cs
        pop     es
        mov     dx,OFFSET hk2F
        mov     ax,252Fh
        int     21h
        mov     word ptr [n2F],0
        mov     ax,MPX_ID * 256
        xor     bx,bx
        int     2Fh
        mov     [t_ax],ax
        mov     [t_bx],bx
        mov     ax,4300h                ; XMS: the kernel's, past the hook
        int     2Fh
        mov     [t_cx],ax
        push    ds
        lds     dx,[old2F]
        mov     ax,252Fh
        int     21h
        pop     ds
        cmp     byte ptr [t_ax],0FFh
        jne     th2f_bad
        cmp     word ptr [t_bx],'KH'
        jne     th2f_bad
        cmp     byte ptr [t_cx],80h
        jne     th2f_bad
        cmp     word ptr [n2F],2
        jne     th2f_bad
        mov     dx,OFFSET m_h2F
        jmp     pass
th2f_bad:
        mov     dx,OFFSET m_h2F
        jmp     fail

;--- the vector a TSR chains to can be called as a far procedure --------
t_chain:
        mov     ah,19h
        int     21h
        mov     [t_ax],ax
        mov     ax,3521h
        int     21h
        mov     word ptr [old21],bx
        mov     word ptr [old21+2],es
        push    cs
        pop     es
        mov     ah,19h
        pushf
        call    dword ptr [old21]
        cmp     al,byte ptr [t_ax]
        jne     tch_bad
        mov     dx,OFFSET m_chain
        jmp     pass
tch_bad:
        mov     dx,OFFSET m_chain
        jmp     fail

;--- INT 29h: to the screen, where it can be looked for -----------------
t_int29:
        mov     si,OFFSET s29
t29_out:
        lodsb
        or      al,al
        jz      t29_look
        int     29h
        jmp     short t29_out
t29_look:
        push    es
        mov     ax,0B800h
        mov     es,ax
        xor     di,di
        mov     cx,2000 - 3
t29_scan:
        cmp     byte ptr es:[di],'h'
        jne     t29_next
        cmp     byte ptr es:[di+2],'2'
        jne     t29_next
        cmp     byte ptr es:[di+4],'9'
        jne     t29_next
        cmp     byte ptr es:[di+6],'k'
        je      t29_found
t29_next:
        add     di,2
        loop    t29_scan
        pop     es
        mov     dx,OFFSET m_i29
        jmp     fail
t29_found:
        pop     es
        mov     al,13                   ; and off the line it is on
        int     29h
        mov     al,10
        int     29h
        mov     dx,OFFSET m_i29
        jmp     pass

;--- INT 25h: sector 0, in registers and as a packet ---------------------
t_int25:
        mov     ah,19h
        int     21h
        mov     [drv],al
        mov     di,OFFSET secbuf
        mov     cx,2048
        xor     ax,ax
        rep     stosw
        mov     [t_sp],sp
        mov     al,[drv]
        mov     cx,1
        xor     dx,dx
        mov     bx,OFFSET secbuf
        int     25h
        jc      t25_err
        mov     [t_sp2],sp
        popf
        mov     ax,[t_sp]
        sub     ax,2
        cmp     ax,[t_sp2]
        jne     t25_bad                 ; the flags were not left
        cmp     word ptr [secbuf+510],0AA55h
        jne     t25_bad
        ; the packet form: CX = FFFFh, DS:BX -> start, count, buffer
        mov     di,OFFSET secbuf2
        mov     cx,2048
        xor     ax,ax
        rep     stosw
        mov     dword ptr [pkt],0
        mov     word ptr [pkt+4],1
        mov     word ptr [pkt+6],OFFSET secbuf2
        mov     word ptr [pkt+8],cs
        mov     al,[drv]
        mov     cx,0FFFFh
        mov     bx,OFFSET pkt
        int     25h
        jc      t25_err
        popf
        mov     si,OFFSET secbuf
        mov     di,OFFSET secbuf2
        mov     cx,256
        repe    cmpsw
        jne     t25_bad
        mov     dx,OFFSET m_i25
        jmp     pass
t25_err:
        popf
t25_bad:
        mov     dx,OFFSET m_i25
        jmp     fail

;--- INT 26h: the same sector back, unchanged, and read again -------------
t_int26:
        mov     al,[drv]
        mov     cx,1
        xor     dx,dx
        mov     bx,OFFSET secbuf
        int     26h
        jc      t26_err
        popf
        mov     di,OFFSET secbuf2
        mov     cx,2048
        xor     ax,ax
        rep     stosw
        mov     al,[drv]
        mov     cx,1
        xor     dx,dx
        mov     bx,OFFSET secbuf2
        int     25h
        jc      t26_err
        popf
        mov     si,OFFSET secbuf
        mov     di,OFFSET secbuf2
        mov     cx,256
        repe    cmpsw
        jne     t26_bad
        mov     dx,OFFSET m_i26
        jmp     pass
t26_err:
        popf
t26_bad:
        mov     dx,OFFSET m_i26
        jmp     fail

;--- ^C: the handler IRETs, and the read happens again -------------------
t_cc_iret:
        mov     dx,OFFSET cc_iret
        mov     ax,2D78h                ; x
        mov     di,OFFSET m_cciret
        jmp     short cc_carryon
t_cc_retf2:
        mov     dx,OFFSET cc_retf2
        mov     ax,1579h                ; y
        mov     di,OFFSET m_ccretf2
        jmp     short cc_carryon
t_cc_retfnc:
        mov     dx,OFFSET cc_retfnc
        mov     ax,2C7Ah                ; z
        mov     di,OFFSET m_ccretfnc
cc_carryon:
        push    di
        push    ax
        mov     ax,2523h
        int     21h
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_CTRLC
        call    kb_stuff
        pop     ax
        push    ax
        call    kb_stuff
        mov     ah,08h                  ; 08h looks for ^C
        int     21h
        mov     [t_ax],ax
        call    put23
        pop     ax
        pop     dx
        cmp     byte ptr [t_ax],al
        jne     fail
        cmp     word ptr [ncc],1
        jne     fail
        jmp     pass

;--- ^C: the handler asks for a different function ------------------------
t_cc_newah:
        mov     dx,OFFSET cc_newah
        mov     ax,2523h
        int     21h
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_CTRLC
        call    kb_stuff
        mov     ah,08h
        int     21h                     ; ...and 30h answers it
        mov     [t_ax],ax
        call    put23
        mov     ah,30h
        int     21h
        mov     dx,OFFSET m_ccnewah
        cmp     ax,[t_ax]
        jne     fail
        cmp     word ptr [ncc],1
        jne     fail
        jmp     pass

;--- ^C: RETF with the carry set ends the program, and only it ------------
t_cc_kill:
        mov     al,'K'
        call    run_child
        mov     dx,OFFSET m_cckill
        jc      fail
        cmp     ax,0100h                ; AH 1: ended by ^C
        jne     fail
        jmp     pass

;--- ^C with no handler of its own: the default ends it -------------------
t_cc_default:
        mov     al,'D'
        call    run_child
        mov     dx,OFFSET m_ccdef
        jc      fail
        cmp     ax,0100h
        jne     failax
        jmp     pass

;--- its own parent: 4Ch sends it to PSP:0Ah and it carries on ------------
t_selfpar:
        mov     al,'P'
        call    run_child
        mov     dx,OFFSET m_selfpar
        jc      fail
        cmp     ax,0007h                ; what it exits with once it is
        jne     fail                    ; back from its terminate address
        jmp     pass

;--- INT 24h hooked by a child is put back when it ends -------------------
t_vec24:
        mov     ax,3524h
        int     21h
        mov     word ptr [t_vec],bx
        mov     word ptr [t_vec+2],es
        push    cs
        pop     es
        mov     al,'V'
        call    run_child
        mov     dx,OFFSET m_vec24
        jc      fail
        mov     ax,3524h
        int     21h
        mov     ax,es
        push    cs
        pop     es
        cmp     bx,word ptr [t_vec]
        jne     fail
        cmp     ax,word ptr [t_vec+2]
        jne     fail
        jmp     pass

;--- 0Bh: a key in the buffer is a key waiting --------------------------
; DOSKEY waits for its keys this way - 0Bh until one is there, then 08h
; - and never blocks in a read at all.
t_status:
        call    kb_flush
        mov     ah,0Bh
        int     21h
        mov     [t_ax],ax
        mov     ax,1E41h                ; A
        call    kb_stuff
        mov     ah,0Bh
        int     21h
        mov     [t_bx],ax
        mov     ah,08h
        int     21h
        mov     [t_cx],ax
        mov     dx,OFFSET m_status
        cmp     byte ptr [t_ax],0       ; nothing waiting first
        jne     fail
        cmp     byte ptr [t_bx],0FFh    ; then the A
        jne     fail
        cmp     byte ptr [t_cx],'A'
        jne     fail
        jmp     pass

;--- INT 2Fh AH=12h: DOS's own services, the ones a program uses -------
t_dos12:
        mov     ax,1200h                ; is DOS there?
        int     2Fh
        cmp     al,0FFh
        jne     t12_bad
        mov     ax,'n'                 ; 1213h: upper case, the argument
        push    ax                      ; on the stack - COMMAND.COM's way
        mov     ax,1213h
        int     2Fh
        pop     bx
        cmp     al,'N'
        jne     t12_bad
        mov     ax,'/'                 ; 1204h: a path separator?
        push    ax
        mov     ax,1204h
        int     2Fh
        pop     bx
        jnz     t12_bad
        cmp     al,'\'
        jne     t12_bad
        mov     di,OFFSET s12a          ; 1212h: the length, NUL and all
        push    ax
        mov     ax,1212h
        int     2Fh
        pop     bx
        cmp     cx,16
        jne     t12_bad
        mov     si,OFFSET s12a          ; 1211h: a name, the way DOS keeps it
        mov     di,OFFSET s12buf
        push    ax
        mov     ax,1211h
        int     2Fh
        pop     bx
        cmp     si,OFFSET s12a + 16     ; both left past their NULs
        jne     t12_bad
        mov     si,OFFSET s12buf
        mov     di,OFFSET s12want
        mov     cx,16
        repe    cmpsb
        jne     t12_bad
        mov     si,OFFSET s12a          ; 121Eh: the same name, DOS's rules
        mov     di,OFFSET s12b
        push    ax
        mov     ax,121Eh
        int     2Fh
        pop     bx
        jnz     t12_bad
        mov     si,OFFSET s12a          ; 121Ah: its drive, C: = 3
        push    ax
        mov     ax,121Ah
        int     2Fh
        pop     bx
        cmp     al,3
        jne     t12_bad
        cmp     si,OFFSET s12a + 2
        jne     t12_bad
        mov     dx,OFFSET m_dos12
        jmp     pass
t12_bad:
        mov     dx,OFFSET m_dos12
        jmp     fail

;=====================================================================
; The children.  Each is one case; what it exits with is the answer.
;=====================================================================

;--- K: a handler that says end me ---------------------------------------
child_kill:
        mov     dx,OFFSET cc_kill
        mov     ax,2523h
        int     21h
        call    kb_flush
        mov     ax,KEY_CTRLC
        call    kb_stuff
child_read:
        mov     ah,08h
        int     21h
        mov     ah,4Ch                  ; still here: the case failed, and
        int     21h                     ; the key it read instead says why
cc_kill:
        stc
        retf

;--- D: no handler; the one it inherited is the box's own -----------------
; An N goes in behind the ^C, for the shell that started us: a COMMAND.COM
; running a batch file asks "Terminate batch job (Y/N)?" when a child
; ends by ^C, and the answer to that is waiting for it.
child_default:
        mov     dx,OFFSET m_childd
        call    puts
        call    kb_flush
        mov     ax,KEY_CTRLC
        call    kb_stuff
        mov     ax,0C2A1h               ; the answerer, if it is resident,
        int     2Fh                     ; types the N once the question is
        cmp     al,0FFh                 ; asked - a shell that flushes the
        je      child_read              ; keyboard first would lose this one
        mov     ax,314Eh                ; N
        call    kb_stuff
        jmp     short child_read

;--- W: not a case - the text screen, to standard output, for reading what
;--- a headless run put on it
show_screen:
        push    ds
        mov     ax,0B800h
        mov     ds,ax
        xor     si,si
        mov     cx,25
ssc_row:
        push    cx
        mov     cx,80
ssc_col:
        lodsw
        mov     dl,al
        mov     ah,02h
        int     21h
        loop    ssc_col
        mov     dl,13
        int     21h
        mov     dl,10
        int     21h
        pop     cx
        loop    ssc_row
        pop     ds
        mov     ax,4C00h
        int     21h

;--- T: not a case - CCFOR and Enter, for the COMMAND.COM that runs next
stuff_ccfor:
        call    kb_flush
        mov     si,OFFSET forkeys
        jmp     ss_one
forkeys     dw  2E43h, 2E43h, 2146h, 184Fh, 1352h, 1C0Dh, 0
;--- B: not a case - how many keys are waiting in the BIOS buffer, and
;--- what 0Bh says about it
show_keys:
        mov     dx,OFFSET m_keys
        call    puts
        push    es
        mov     ax,40h
        mov     es,ax
        mov     ax,es:[1Ch]             ; tail - head, in words, round the
        sub     ax,es:[1Ah]             ; ring
        jns     sk_pos
        add     ax,es:[82h]
        sub     ax,es:[80h]
sk_pos:
        shr     ax,1
        pop     es
        call    putdec
        mov     dx,OFFSET m_keys0B
        call    puts
        mov     ah,0Bh
        int     21h
        call    putbyte
        mov     dx,OFFSET m_crlf
        call    puts
        mov     ax,4C00h
        int     21h

;--- L: not a case - a spy.  Goes resident on INT 21h and 2Fh and prints
;--- every call's AH (21h) or AX (2Fh) to LPT1 through the BIOS, a line
;--- at a time, so a headless run with the printer captured shows what a
;--- program in a box asked for, in order, up to the moment it stopped.
logger:
        mov     ax,3521h
        int     21h
        mov     word ptr [lg_old21],bx
        mov     word ptr [lg_old21+2],es
        mov     ax,352Fh
        int     21h
        mov     word ptr [lg_old2F],bx
        mov     word ptr [lg_old2F+2],es
        push    cs
        pop     es
        mov     dx,OFFSET lg21
        mov     ax,2521h
        int     21h
        mov     dx,OFFSET lg2F
        mov     ax,252Fh
        int     21h
        mov     dx,OFFSET prog_end
        add     dx,STACK_BYTES + 15
        shr     dx,4
        mov     ax,3100h
        int     21h

lg_old21    dd  0
lg_old2F    dd  0

lg21:
        push    ax
        push    dx
        mov     dl,'1'
        call    lg_tag
        mov     al,ah
        call    lg_byte
        call    lg_eol
        pop     dx
        pop     ax
        jmp     dword ptr cs:[lg_old21]
lg2F:
        push    ax
        push    dx
        push    ax
        mov     dl,'F'
        call    lg_tag
        pop     ax
        push    ax
        mov     al,ah
        call    lg_byte
        pop     ax
        call    lg_byte
        call    lg_eol
        pop     dx
        pop     ax
        jmp     dword ptr cs:[lg_old2F]

; lg_tag - "21:" or "2F:" (DL = the second digit), out
lg_tag:
        push    ax
        mov     al,'2'
        call    lg_ch
        mov     al,dl
        call    lg_ch
        mov     al,':'
        call    lg_ch
        pop     ax
        ret
; lg_byte - AL as two hex digits
lg_byte:
        push    ax
        shr     al,4
        call    lg_hex
        pop     ax
        and     al,0Fh
lg_hex:
        add     al,'0'
        cmp     al,'9'
        jbe     lg_ch
        add     al,7
; lg_ch - AL to LPT1, by the BIOS
lg_ch:
        push    ax
        push    dx
        mov     ah,00h
        xor     dx,dx
        int     17h
        pop     dx
        pop     ax
        ret
lg_eol:
        push    ax
        mov     al,' '
        call    lg_ch
        pop     ax
        ret

; H: VER, Enter, Up, Enter, EXIT - DOSKEY's history, if a DOSKEY loaded
;    in a box is answering the shell's INT 2Fh AX=4810h
stuff_ver:
        call    kb_flush
        mov     si,OFFSET verkeys
        jmp     ss_one
verkeys     dw  2F56h, 1245h, 1352h, 1C0Dh, 4800h, 1C0Dh
            dw  1245h, 2D58h, 1749h, 1454h, 1C0Dh, 0
; U: CC1 and Enter - the same, with one child and no FOR
stuff_cc1:
        call    kb_flush
        mov     si,OFFSET cc1keys
        jmp     ss_one
cc1keys     dw  2E43h, 2E43h, 0231h, 1C0Dh, 0
; C: a Ctrl+C and nothing else, for whatever program asks for a key
;    next - XCTEST's XCOPY /W, which has to end with errorlevel 2
stuff_ctrlc:
        call    kb_flush
        mov     si,OFFSET ccleys
        jmp     ss_one
ccleys      dw  KEY_CTRLC, 0

;--- I: not a case - go resident as the answerer.  A shell asks
;--- "Terminate batch job (Y/N)?" with the keyboard flushed first, as
;--- 6.22 does, so an N typed ahead is thrown away.  This types one
;--- from INT 28h instead - DOS's idle call, which comes while the
;--- shell is waiting for the key - once for every time a child arms it
;--- through INT 2Fh AX=C2A1h.  AX=C2A0h asks whether it is there.  The
;--- whole program stays: it is a test, and eleven K is nothing.
answerer:
        mov     ax,3528h
        int     21h
        mov     word ptr [ans_old28],bx
        mov     word ptr [ans_old28+2],es
        mov     ax,352Fh
        int     21h
        mov     word ptr [ans_old2F],bx
        mov     word ptr [ans_old2F+2],es
        push    cs
        pop     es
        mov     dx,OFFSET ans28
        mov     ax,2528h
        int     21h
        mov     dx,OFFSET ans2F
        mov     ax,252Fh
        int     21h
        push    es
        xor     ax,ax
        mov     es,ax
        mov     ax,es:[46Ch]
        mov     [ans_t0],ax
        pop     es
        mov     dx,OFFSET m_ans
        call    puts
        mov     dx,OFFSET prog_end
        add     dx,STACK_BYTES + 15
        shr     dx,4
        mov     ax,3100h
        int     21h

ans_old28   dd  0
ans_old2F   dd  0
ans_armed   dw  0
ans_t0      dw  0
ans_shot    db  0
ans_row     db  82 dup(0)
ans_name    db  'C:\SCREEN.TXT',0

ans28:
        cmp     word ptr cs:[ans_armed],0
        je      a28_on
        push    ax
        push    cx
        mov     ah,01h
        int     16h
        jnz     a28_pop                 ; a key is waiting: not our moment
        mov     ah,05h
        mov     cx,314Eh
        int     16h
        dec     word ptr cs:[ans_armed]
a28_pop:
        pop     cx
        pop     ax
a28_on:
        ; AND A SNAPSHOT OF THE SCREEN, once, forty-five seconds in: what
        ; a headless run was showing when it stopped moving.  INT 28h is
        ; the moment DOS allows a resident program its file calls.
        cmp     byte ptr cs:[ans_shot],0
        jne     a28_chain
        pusha
        push    ds
        push    es
        xor     ax,ax
        mov     es,ax
        mov     ax,es:[46Ch]
        sub     ax,cs:[ans_t0]
        cmp     ax,18 * 45
        jb      a28_noshot
        mov     byte ptr cs:[ans_shot],1
        push    cs
        pop     ds
        mov     dx,OFFSET ans_name
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      a28_noshot
        mov     bx,ax
        mov     ax,0B800h
        mov     es,ax
        xor     si,si
        mov     bp,25
a28_rowl:
        mov     di,OFFSET ans_row
        mov     cx,80
a28_cell:
        mov     al,es:[si]
        mov     [di],al
        add     si,2
        inc     di
        loop    a28_cell
        mov     word ptr [di],0A0Dh
        mov     dx,OFFSET ans_row
        mov     cx,82
        mov     ah,40h
        int     21h
        dec     bp
        jnz     a28_rowl
        mov     ah,3Eh
        int     21h
        mov     ah,0Dh                  ; and onto the disk, since the run
        int     21h                     ; will end by being stopped
a28_noshot:
        pop     es
        pop     ds
        popa
a28_chain:
        jmp     dword ptr cs:[ans_old28]

ans2F:
        cmp     ax,0C2A0h
        je      a2f_here
        cmp     ax,0C2A1h
        je      a2f_arm
        jmp     dword ptr cs:[ans_old2F]
a2f_arm:
        inc     word ptr cs:[ans_armed]
a2f_here:
        mov     al,0FFh
        iret

;--- Q: not a case - the DOS vectors as they stand, for finding out which
;--- program left one pointing somewhere it should not
show_vecs:
        mov     dx,OFFSET m_vecs
        call    puts
        mov     si,OFFSET qvecs
sv_one:
        lodsb
        or      al,al
        jz      sv_done
        push    ax
        call    putbyte
        mov     dl,'='
        mov     ah,02h
        int     21h
        pop     ax
        mov     ah,35h
        int     21h
        mov     ax,es
        call    puthex
        mov     dl,':'
        mov     ah,02h
        int     21h
        mov     ax,bx
        call    puthex
        mov     dl,' '
        mov     ah,02h
        int     21h
        push    cs
        pop     es
        jmp     short sv_one
sv_done:
        mov     dx,OFFSET m_ppsp        ; and what the parent's PSP saved
        call    puts
        push    ds
        mov     ax,word ptr ds:[16h]
        call    puthex
        mov     ds,ax
        mov     ax,word ptr ds:[10h]
        mov     bx,word ptr ds:[0Eh]
        pop     ds
        push    ax
        mov     dl,' '
        mov     ah,02h
        int     21h
        pop     ax
        call    puthex
        mov     dl,':'
        mov     ah,02h
        int     21h
        mov     ax,bx
        call    puthex
        mov     dx,OFFSET m_crlf
        call    puts
        mov     ax,4C00h
        int     21h
qvecs       db  20h, 21h, 22h, 23h, 24h, 28h, 29h, 2Fh, 0

;--- S: not a case - keys for the COMMAND.COM that runs next --------------
; ^C at its prompt, then CCALIVE - a batch file that says the shell is
; still there - and EXIT.  Fourteen keys: the BIOS buffer holds fifteen.
stuff_shell:
        call    kb_flush
        mov     si,OFFSET shkeys
ss_one:
        lodsw
        or      ax,ax
        jz      ss_done
        call    kb_stuff
        jmp     short ss_one
ss_done:
        mov     ax,4C00h
        int     21h
shkeys      dw  KEY_CTRLC
            dw  2E43h, 2E43h, 1E41h, 264Ch, 1749h, 2F56h, 1245h, 1C0Dh
            dw  1245h, 2D58h, 1749h, 1454h, 1C0Dh, 0

;--- P: its own parent ------------------------------------------------------
child_selfpar:
        mov     ax,word ptr ds:[16h]
        mov     [sp_parent],ax
        mov     ax,word ptr ds:[0Ah]
        mov     word ptr [sp_term],ax
        mov     ax,word ptr ds:[0Ch]
        mov     word ptr [sp_term+2],ax
        mov     word ptr ds:[16h],cs
        mov     word ptr ds:[0Ah],OFFSET sp_back
        mov     word ptr ds:[0Ch],cs
        mov     ax,4C05h                ; 5 if it really ends here
        int     21h
sp_back:
        push    cs
        pop     ds
        mov     ax,[sp_parent]          ; a real exit, now: our parent
        mov     word ptr ds:[16h],ax    ; and our terminate address back
        mov     ax,word ptr [sp_term]
        mov     word ptr ds:[0Ah],ax
        mov     ax,word ptr [sp_term+2]
        mov     word ptr ds:[0Ch],ax
        mov     ax,4C07h
        int     21h

;--- V: hook INT 24h and leave without unhooking it --------------------------
child_vec24:
        mov     dx,OFFSET child_vec24   ; anywhere in us: it is never called
        mov     ax,2524h
        int     21h
        mov     ax,4C00h
        int     21h

;=====================================================================
; Helpers
;=====================================================================

; run_child - AL = the case letter.  CF if 4Bh failed; else AX = 4Dh.
run_child:
        mov     byte ptr [ctail+3],al
        mov     word ptr [pb_env],0
        mov     word ptr [pb_tail],OFFSET ctail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs
        mov     dx,OFFSET self
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        push    cs
        pop     ds
        push    cs
        pop     es
        jc      rc_out
        mov     ah,4Dh
        int     21h
        clc
rc_out:
        ret

; find_self - this program's own path, out of the end of its
;             environment, into [self]
find_self:
        push    ds
        mov     ax,word ptr ds:[2Ch]
        mov     ds,ax
        xor     si,si
fs_var:
        cmp     byte ptr [si],0
        je      fs_end
fs_skip:
        lodsb
        or      al,al
        jnz     fs_skip
        jmp     short fs_var
fs_end:
        add     si,3                    ; the NUL and the string count
        mov     di,OFFSET self
fs_copy:
        lodsb
        mov     cs:[di],al
        inc     di
        or      al,al
        jnz     fs_copy
        pop     ds
        ret

; kb_flush - empty the BIOS keyboard buffer
kb_flush:
        mov     ah,01h
        int     16h
        jz      kf_out
        mov     ah,00h
        int     16h
        jmp     short kb_flush
kf_out:
        ret

; kb_stuff - AX = a key, into the BIOS keyboard buffer
kb_stuff:
        mov     cx,ax
        mov     ah,05h
        int     16h
        ret

; put23 - ^C's vector back to what the box began with
put23:
        push    ds
        lds     dx,[orig23]
        mov     ax,2523h
        int     21h
        pop     ds
        ret

; failax - fail, and say what 4Dh gave back: AX
failax:
        push    ax
        call    fail
        mov     dx,OFFSET m_gave
        call    puts
        pop     ax
        call    puthex
        mov     dx,OFFSET m_crlf
        call    puts
        ret

; pass / fail - DX = the case's name; a line either way
pass:
        push    dx
        mov     dx,OFFSET m_ok
        call    puts
        pop     dx
        call    puts
        ret
fail:
        push    dx
        mov     dx,OFFSET m_fail
        call    puts
        pop     dx
        call    puts
        inc     word ptr [nfail]
        ret

; puts - the ASCIIZ at DX, to standard output
puts:
        push    ax
        push    bx
        push    cx
        push    si
        mov     si,dx
        xor     cx,cx
pu_len:
        cmp     byte ptr [si],0
        je      pu_go
        inc     si
        inc     cx
        jmp     short pu_len
pu_go:
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

; putbyte - AL as two hex digits
putbyte:
        push    ax
        mov     ah,al
        xor     al,al
        push    cx
        push    dx
        mov     cx,2
        jmp     short ph_one

; puthex - AX as four hex digits
puthex:
        push    ax
        push    cx
        push    dx
        mov     cx,4
ph_one:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph_dig
        add     al,7
ph_dig:
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     ax
        loop    ph_one
        pop     dx
        pop     cx
        pop     ax
        ret

; putdec - AX in decimal
putdec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,10
        xor     cx,cx
pd_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     pd_div
pd_out:
        pop     dx
        add     dl,'0'
        mov     ah,02h
        int     21h
        loop    pd_out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; Data
;=====================================================================
m_hi        db  'HOOKTEST: the DOS vectors through a program',39,'s own hooks, and ^C',13,10,0
m_ok        db  '  ok    ',0
m_fail      db  '  FAIL  ',0
m_h21       db  'INT 21h went through a hook and on to DOS',13,10,0
m_h2F       db  'INT 2Fh went through a hook, and past it to the kernel',39,'s XMS',13,10,0
m_chain     db  'the INT 21h vector answers PUSHF/CALL FAR, as a TSR chains',13,10,0
m_i29       db  'INT 29h reached the screen',13,10,0
m_i25       db  'INT 25h read sector 0, both forms, flags left on the stack',13,10,0
m_i26       db  'INT 26h wrote it back and it reads the same',13,10,0
m_cciret    db  '^C, handler IRETs: the read is made again',13,10,0
m_ccretf2   db  '^C, handler RETF 2: the read is made again',13,10,0
m_ccretfnc  db  '^C, handler RETF with carry clear: the read is made again',13,10,0
m_ccnewah   db  '^C, handler changes AH: that function is made instead',13,10,0
m_cckill    db  '^C, handler RETF with carry set: the child ends, 4Dh says ^C',13,10,0
m_ccdef     db  '^C, no handler: the default ends the child, 4Dh says ^C',13,10,0
m_selfpar   db  'its own parent: 4Ch went to PSP:0Ah and the program carried on',13,10,0
m_vec24     db  'INT 24h hooked by a child is put back when it ends',13,10,0
m_vecs      db  '  vectors ',0
m_ppsp      db  'parent ',0
m_gave      db  '        4Dh gave ',0
m_crlf      db  13,10,0
m_ans       db  '  the Terminate-batch-job answerer is resident',13,10,0
m_keys      db  '  keys waiting: ',0
m_keys0B    db  ', and 0Bh says ',0
m_childd    db  '  a child that ^C is about to end',13,10,0
m_status    db  '0Bh sees a key in the buffer, and 08h reads it',13,10,0
m_dos12     db  'INT 2Fh AH=12h: install, 04h, 11h, 12h, 13h, 1Ah and 1Eh answer as DOS does',13,10,0
s12a        db  'c:/Dir/File.txt',0
s12b        db  'C:\dir\FILE.TXT',0
s12want     db  'C:\DIR\FILE.TXT',0
s12buf      db  20 dup(0)
m_done      db  'HOOKTEST: ',0
m_fails     db  ' failed',13,10,0
s29         db  'h29k',0

nfail       dw  0
t_ax        dw  0
t_bx        dw  0
t_cx        dw  0
t_sp        dw  0
t_sp2       dw  0
t_vec       dd  0
drv         db  0
            db  0
sp_parent   dw  0
sp_term     dd  0
pkt         dd  0                       ; first sector
            dw  0                       ; count
            dd  0                       ; buffer
ctail       db  3,' /X',13
pb_env      dw  0
pb_tail     dd  0
pb_fcb1     dd  0
pb_fcb2     dd  0
self        db  128 dup(0)
secbuf      db  4096 dup(0)
secbuf2     db  4096 dup(0)
prog_end    LABEL BYTE

_HK     ENDS
        END     start
