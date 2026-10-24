;=====================================================================
; BRKTEST.ASM - BRKTEST.COM: BREAK, and the ^C nobody is reading.
;
; A ^C typed while a program is reading the keyboard has always been
; acted on.  This is about the other times DOS looks for one, which are
; what BREAK is:
;
;   BREAK OFF - every 64th character through 02h, 09h or a write to
;     CON in cooked mode, and 0Bh.  Not a write to anything else, not
;     CON in raw mode, and not a call that is not a character call.
;   BREAK ON  - all that, and every call but the character calls and a
;     few DOS answers before it is busy (33h among them).
;   Either way, only the key at the FRONT of the buffer: a ^C typed
;   behind another key waits until that key has been read.
;
; And Ctrl-Break, which the kernel's keyboard takes as the BIOS does:
; the buffer emptied, bit 7 of 40:71 set, INT 1Bh called, 0000h left in
; it.  DOS's own INT 1Bh marks the key and DOS reads the mark as a ^C,
; whatever was typed; the 0000h is left for the console to drop, as
; MS-DOS leaves it.  A program's own INT 1Bh is called instead, and
; there is a ^C only if it passes the call on.
;
; And ^S and ^P, which DOS takes at the same moments as a ^C, BREAK ON
; or OFF, and while 01h, 08h or 0Ah waits: ^S holds everything until
; the next key, which it takes too, and ^P (or Ctrl+PrtSc) turns the
; printer echo over.  06h and 07h read them as keys.  The echo itself
; is not looked at here - there is no printer to read back - and each
; ^P case presses it twice, so that it ends off.
;
; This runs in a DOS box, so a ^C goes to its INT 23h the way DOS sends
; it.  The handler here counts and IRETs, and DOS then makes the call
; again.  Keys are put in the buffer with INT 16h AH=05h, and Ctrl-Break
; is pressed with 21h/F1h AL=23h, which takes scan codes as the
; keyboard sends them.
;
; EACH CASE PRINTS ONLY WHEN NOTHING IS WAITING.  Its own report goes
; out through 09h, and 09h looks for a ^C too: a line printed with one
; still in the buffer could take it before the call being tested does.
;
; BRK32.EXE is the same for a native program.  The exit code is the
; number of failures.
;
; Run it on the parked DOS too:  PARKTEST \TESTS\BRKTEST.COM
; RMDOS keeps the same rules.  The six Ctrl-Break cases fail there,
; because 21h/F1h is the kernel's and nothing presses the key.  The
; first line says whether BREAK came across from the kernel.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_BT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_BT, DS:_BT, ES:_BT, SS:_BT
        ORG     0

KEY_CTRLC       EQU     2E03h           ; scan code and ASCII of Ctrl+C
KEY_CTRLS       EQU     1F13h
KEY_CTRLP       EQU     1910h
KEY_PRTSC       EQU     7200h           ; Ctrl+PrtSc, as the BIOS has it
KEY_ALTR        EQU     1300h           ; Alt+R: its scan code is ^S's ASCII
KEY_A           EQU     1E41h
KEY_X           EQU     2D78h
KEY_Y           EQU     1579h

start:
        mov     dx,OFFSET msg_hi
        call    puts

        mov     ax,3300h                ; BREAK as it was, to put back -
        int     21h                     ; and to say, which is how a run
        mov     [was_brk],dl            ; under RMDOS (PARKTEST) shows
        mov     dx,OFFSET msg_wasoff    ; that the setting went across
        cmp     byte ptr [was_brk],0
        je      st_said
        mov     dx,OFFSET msg_wason
st_said:
        call    puts
        push    es
        push    40h
        pop     es
        mov     al,es:[71h]             ; and the BIOS's break flag
        mov     [was_71],al
        pop     es

        mov     ax,3523h
        int     21h
        mov     word ptr [old23],bx
        mov     word ptr [old23+2],es
        push    cs
        pop     es
        mov     dx,OFFSET my23
        mov     ax,2523h
        int     21h

        call    t_setget
        call    t_offdisk
        call    t_off0b
        call    t_ondisk
        call    t_behind
        call    t_cbreak
        call    t_cbon
        call    t_cbread
        call    t_off02
        call    t_offnul
        call    t_onnul
        call    t_offcon
        call    t_offraw
        call    t_on33
        call    t_own1b
        call    t_pass1b
        call    t_rom1b
        call    t_stop
        call    t_stopc
        call    t_stop02
        call    t_stop08
        call    t_raws
        call    t_ctlp
        call    t_prtsc
        call    t_altr
        call    t_echo

        call    kb_flush                ; nothing left for the shell
        push    ds
        lds     dx,[old23]
        mov     ax,2523h
        int     21h
        pop     ds
        mov     dl,[was_brk]
        mov     ax,3301h
        int     21h
        push    es
        push    40h
        pop     es
        mov     al,[was_71]
        mov     es:[71h],al
        pop     es

        mov     dx,OFFSET msg_end
        call    puts
        mov     ax,[nfail]
        call    putdec
        mov     dx,OFFSET msg_failed
        call    puts
        mov     al,byte ptr [nfail]
        mov     ah,4Ch
        int     21h

;=====================================================================
; The cases.  Each sets up, makes the one call it is about, keeps what
; it saw, empties the buffer, and only then says pass or fail.
;=====================================================================

;--- 3301h and 3300h ---------------------------------------------------
t_setget:
        mov     dl,1
        call    set_brk
        mov     ax,3300h
        int     21h
        mov     [r2],dl
        mov     dl,0
        call    set_brk
        mov     ax,3300h
        int     21h
        mov     [r3],dl
        mov     dl,1                    ; 3302h: set it, and say what it was
        mov     ax,3302h
        int     21h
        mov     byte ptr [r4],dl
        mov     ax,3300h
        int     21h
        mov     byte ptr [r4+1],dl
        mov     dl,0
        call    set_brk
        mov     dx,OFFSET m_setget
        cmp     byte ptr [r2],1
        jne     fail
        cmp     byte ptr [r3],0
        jne     fail
        cmp     word ptr [r4],0100h     ; it was off, and is on
        jne     fail
        jmp     pass

;--- BREAK OFF: a call that is not a character call leaves it ----------
t_offdisk:
        mov     dl,0
        call    arm_cc
        mov     ah,19h                  ; the current drive
        int     21h
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_offdisk
        cmp     word ptr [ncc],0
        jne     fail
        cmp     word ptr [r1],KEY_CTRLC
        jne     fail
        jmp     pass

;--- BREAK OFF: 0Bh does not -------------------------------------------
t_off0b:
        mov     dl,0
        call    arm_cc
        mov     ah,0Bh
        int     21h
        mov     [r2],al
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_off0b
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r1],-1        ; and it was taken
        jne     fail
        cmp     byte ptr [r2],0         ; the call made again: nothing waits
        jne     fail
        jmp     pass

;--- BREAK ON: the same call takes it ----------------------------------
t_ondisk:
        mov     dl,1
        call    arm_cc
        mov     ah,19h
        int     21h
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_ondisk
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r1],-1
        jne     fail
        jmp     pass

;--- BREAK ON: a ^C behind another key waits for it --------------------
t_behind:
        mov     dl,1
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_A
        call    kb_stuff
        mov     ax,KEY_CTRLC
        call    kb_stuff
        mov     ah,19h
        int     21h
        mov     ax,[ncc]
        mov     [r2],al                 ; no ^C yet
        call    front
        mov     [r1],ax                 ; the A in front of it
        mov     ah,07h                  ; the A read, which 07h does
        int     21h                     ; without looking for a ^C
        mov     [r3],al
        mov     ah,19h                  ; and now the ^C is in front
        int     21h
        call    front
        mov     [r4],ax
        call    kb_flush
        mov     dx,OFFSET m_behind
        cmp     byte ptr [r2],0
        jne     fail
        cmp     word ptr [r1],KEY_A
        jne     fail
        cmp     byte ptr [r3],'A'
        jne     fail
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r4],-1
        jne     fail
        jmp     pass

;--- Ctrl-Break, as the BIOS has it ------------------------------------
t_cbreak:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        push    es
        push    40h
        pop     es
        and     byte ptr es:[71h],7Fh
        pop     es
        mov     ax,KEY_X                ; typed ahead, and then thrown away
        call    kb_stuff
        mov     ax,KEY_Y
        call    kb_stuff
        call    press_cbrk
        push    es
        push    40h
        pop     es
        mov     al,es:[71h]
        mov     [r2],al
        pop     es
        call    nwait
        mov     [r3],al
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_cbreak
        test    byte ptr [r2],80h
        jz      fail
        cmp     byte ptr [r3],1
        jne     fail
        cmp     word ptr [r1],0
        jne     fail
        cmp     word ptr [ncc],0
        jne     fail
        jmp     pass

;--- ...which BREAK ON then takes like a ^C ----------------------------
t_cbon:
        mov     dl,1
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_A                ; behind a key, which Ctrl-Break
        call    kb_stuff                ; does not wait for
        call    press_cbrk
        mov     ah,19h
        int     21h
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_cbon
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r1],0         ; the BIOS's 0000h stays behind for
        jne     fail                    ; the console to drop, as in MS-DOS
        jmp     pass

;--- ...and 07h reads as a ^C ------------------------------------------
t_cbread:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        call    press_cbrk
        mov     byte ptr [r2],0FFh      ; nothing there: a failure, and not
        call    front                   ; a wait for ever in 07h
        cmp     ax,-1
        je      tcr_none
        mov     ah,07h
        int     21h
        mov     [r2],al
tcr_none:
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_cbread
        cmp     byte ptr [r2],3
        jne     fail
        cmp     word ptr [r1],0         ; the 03h was DOS's INT 1Bh's mark,
        jne     fail                    ; and the 0000h is still there
        cmp     word ptr [ncc],0
        jne     fail
        jmp     pass

;--- BREAK OFF: 64 characters through 02h take it ----------------------
; Into standard output, so into this test's own output: a row of dots.
t_off02:
        mov     dl,0
        call    arm_cc
        mov     cx,64
to2_lp:
        mov     dl,'.'
        mov     ah,02h
        int     21h
        loop    to2_lp
        call    front
        mov     [r1],ax
        call    kb_flush
        call    crlf
        mov     dx,OFFSET m_off02
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r1],-1
        jne     fail
        jmp     pass

;--- BREAK OFF: 64 bytes to NUL do not ---------------------------------
t_offnul:
        mov     dx,OFFSET nm_nul
        call    open_w
        mov     dl,0
        call    arm_cc
        call    write64
        call    front
        mov     [r1],ax
        call    kb_flush
        call    close_h
        mov     dx,OFFSET m_offnul
        cmp     word ptr [ncc],0
        jne     fail
        cmp     word ptr [r1],KEY_CTRLC
        jne     fail
        jmp     pass

;--- BREAK ON: they do, before the write starts ------------------------
t_onnul:
        mov     dx,OFFSET nm_nul
        call    open_w
        mov     dl,1
        call    arm_cc
        call    write64
        call    front
        mov     [r1],ax
        call    kb_flush
        call    close_h
        mov     dx,OFFSET m_onnul
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r1],-1
        jne     fail
        jmp     pass

;--- BREAK OFF: 64 bytes to CON take it --------------------------------
; They go to the screen, not to this test's output.
t_offcon:
        mov     dx,OFFSET nm_con
        call    open_w
        mov     dl,0
        call    arm_cc
        call    write64
        call    front
        mov     [r1],ax
        call    kb_flush
        call    close_h
        mov     dx,OFFSET m_offcon
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r1],-1
        jne     fail
        jmp     pass

;--- ...and in raw mode they do not ------------------------------------
t_offraw:
        mov     dx,OFFSET nm_con
        call    open_w
        mov     bx,[hnd]
        mov     ax,4400h
        int     21h
        xor     dh,dh
        or      dl,20h                  ; raw
        mov     ax,4401h
        int     21h
        mov     dl,0
        call    arm_cc
        call    write64
        call    front
        mov     [r1],ax
        call    kb_flush
        call    close_h                 ; the mode goes with the handle
        mov     dx,OFFSET m_offraw
        cmp     word ptr [ncc],0
        jne     fail
        cmp     word ptr [r1],KEY_CTRLC
        jne     fail
        jmp     pass

;--- BREAK ON: 33h itself is not broken into ---------------------------
t_on33:
        mov     dl,1
        call    arm_cc
        mov     ax,3300h
        int     21h
        mov     [r2],dl
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_on33
        cmp     word ptr [ncc],0
        jne     fail
        cmp     byte ptr [r2],1
        jne     fail
        cmp     word ptr [r1],KEY_CTRLC
        jne     fail
        jmp     pass

;--- A program's own INT 1Bh is called, and keeps the key --------------
; The call comes as the 21h/F1h that pressed the key returns, which is
; the box's first moment to take an interrupt.  BREAK ON, so 19h would
; take a ^C if there were one.
t_own1b:
        mov     byte ptr [chain1b],0
        call    break1b
        mov     dx,OFFSET m_own1b
        cmp     byte ptr [r2],1
        jne     fail
        cmp     word ptr [ncc],0
        jne     fail
        cmp     word ptr [r1],0         ; the BIOS's 0000h, which DOS drops
        jne     fail
        jmp     pass

;--- ...and one that passes it on gets DOS's ^C ------------------------
t_pass1b:
        mov     byte ptr [chain1b],1
        call    break1b
        mov     dx,OFFSET m_pass1b
        cmp     byte ptr [r2],1
        jne     fail
        cmp     word ptr [ncc],1
        jne     fail
        jmp     pass

;--- With INT 09h this program's, the BIOS's handler does it all -------
; The scan codes go to the box, my9 passes them on, and the BIOS's own
; INT 09h empties the buffer, calls INT 1Bh - DOS's, which marks the
; key - and leaves 0000h.  BREAK ON: 19h takes the mark.
t_rom1b:
        mov     dl,1
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,3509h
        int     21h
        mov     word ptr [old9],bx
        mov     word ptr [old9+2],es
        push    cs
        pop     es
        mov     dx,OFFSET my9
        mov     ax,2509h
        int     21h
        call    press_cbrk
        mov     ah,19h
        int     21h
        call    front
        mov     [r1],ax
        push    ds
        lds     dx,[old9]
        mov     ax,2509h
        int     21h
        pop     ds
        call    kb_flush
        mov     dx,OFFSET m_rom1b
        cmp     word ptr [ncc],1
        jne     fail
        cmp     word ptr [r1],0
        jne     fail
        jmp     pass

; break1b - INT 1Bh hooked (my1b), Ctrl-Break pressed, 19h with BREAK
;           ON, INT 1Bh put back -> r2 = the calls my1b had, r1 = the
;           key at the front
break1b:
        mov     dl,1
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     word ptr [n1b],0
        mov     ax,351Bh
        int     21h
        mov     word ptr [old1b],bx
        mov     word ptr [old1b+2],es
        push    cs
        pop     es
        mov     dx,OFFSET my1b
        mov     ax,251Bh
        int     21h
        call    press_cbrk
        mov     ah,19h
        int     21h
        mov     ax,[n1b]
        mov     [r2],al
        call    front
        mov     [r1],ax
        push    ds
        lds     dx,[old1b]
        mov     ax,251Bh
        int     21h
        pop     ds
        jmp     kb_flush

;--- ^S at 0Bh: taken, with the key that ends the wait -----------------
t_stop:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_CTRLS
        call    kb_stuff
        mov     ax,KEY_A                ; ends the wait, and goes with it
        call    kb_stuff
        mov     ax,KEY_X                ; stays
        call    kb_stuff
        mov     ah,0Bh
        int     21h
        mov     [r2],al
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_stop
        cmp     byte ptr [r2],0FFh
        jne     fail
        cmp     word ptr [r1],KEY_X
        jne     fail
        cmp     word ptr [ncc],0
        jne     fail
        jmp     pass

;--- ^S and then ^C: the wait ends in a break --------------------------
t_stopc:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_CTRLS
        call    kb_stuff
        mov     ax,KEY_CTRLC
        call    kb_stuff
        mov     ah,0Bh
        int     21h
        mov     [r2],al
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_stopc
        cmp     word ptr [ncc],1
        jne     fail
        cmp     byte ptr [r2],0         ; the call made again: nothing waits
        jne     fail
        cmp     word ptr [r1],-1
        jne     fail
        jmp     pass

;--- 64 characters through 02h hold for ^S -----------------------------
t_stop02:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_CTRLS
        call    kb_stuff
        mov     ax,KEY_A
        call    kb_stuff
        mov     cx,64
ts2_lp:
        mov     dl,'.'
        mov     ah,02h
        int     21h
        loop    ts2_lp
        call    front
        mov     [r1],ax
        call    kb_flush
        call    crlf
        mov     dx,OFFSET m_stop02
        cmp     word ptr [ncc],0
        jne     fail
        cmp     word ptr [r1],-1        ; both keys taken
        jne     fail
        jmp     pass

;--- 08h: ^S and the key after it are DOS's, and the next is read ------
t_stop08:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     ax,KEY_CTRLS
        call    kb_stuff
        mov     ax,KEY_A
        call    kb_stuff
        mov     ax,KEY_Y
        call    kb_stuff
        mov     ah,08h
        int     21h
        mov     [r2],al
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_stop08
        cmp     byte ptr [r2],'y'
        jne     fail
        cmp     word ptr [r1],-1
        jne     fail
        jmp     pass

;--- 07h and 06h read ^S as a key --------------------------------------
t_raws:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     ax,KEY_CTRLS
        call    kb_stuff
        mov     ax,KEY_CTRLS
        call    kb_stuff
        mov     ah,07h
        int     21h
        mov     [r2],al
        mov     byte ptr [r3],0
        mov     dl,0FFh
        mov     ah,06h
        int     21h
        jz      trs_none
        mov     [r3],al
trs_none:
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_raws
        cmp     byte ptr [r2],13h
        jne     fail
        cmp     byte ptr [r3],13h
        jne     fail
        cmp     word ptr [r1],-1
        jne     fail
        jmp     pass

;--- ^P at 0Bh: taken, twice, so that the echo ends as it began --------
t_ctlp:
        mov     ax,KEY_CTRLP
        mov     dx,OFFSET m_ctlp
        jmp     short ctlp_two
t_prtsc:
        mov     ax,KEY_PRTSC            ; ...and Ctrl+PrtSc the same
        mov     dx,OFFSET m_prtsc
ctlp_two:
        mov     [r4],ax
        mov     [msgp],dx
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     ax,[r4]
        call    kb_stuff
        mov     ax,KEY_X
        call    kb_stuff
        mov     ah,0Bh                  ; the echo on
        int     21h
        mov     [r2],al
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     ax,[r4]
        call    kb_stuff
        mov     ah,0Bh                  ; and off
        int     21h
        mov     [r3],al
        call    front
        mov     [r4],ax
        call    kb_flush
        mov     dx,[msgp]
        cmp     byte ptr [r2],0FFh      ; the X after it still waits
        jne     fail
        cmp     word ptr [r1],KEY_X
        jne     fail
        cmp     byte ptr [r3],0
        jne     fail
        cmp     word ptr [r4],-1
        jne     fail
        jmp     pass

;--- Alt+R's scan code is a character to 08h, not a ^S -----------------
t_altr:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     ax,KEY_ALTR
        call    kb_stuff
        mov     ax,KEY_X
        call    kb_stuff
        mov     ah,08h
        int     21h
        mov     [r2],al
        mov     ah,08h                  ; 13h, owed, with the X behind it
        int     21h
        mov     [r3],al
        call    front
        mov     [r1],ax
        call    kb_flush
        mov     dx,OFFSET m_altr
        cmp     byte ptr [r2],0
        jne     fail
        cmp     byte ptr [r3],13h
        jne     fail
        cmp     word ptr [r1],KEY_X
        jne     fail
        jmp     pass

;--- ^P: a line to the screen goes to the printer as well --------------
; Nothing here can read the printer back, so this judges nothing: under
; RUNTESTS the line is in LPT.TXT, DBXRUN's capture of LPT1.  Standard
; output is this test's .OUT file, and the echo is only while handle 1
; is a device, as MS-DOS has it - so CON is handle 1 for the line.
t_echo:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     dx,OFFSET nm_con
        call    open_w
        cmp     word ptr [hnd],-1
        je      te_out
        mov     bx,1                    ; handle 1, kept
        mov     ah,45h
        int     21h
        jc      te_close
        mov     [r4],ax
        mov     bx,[hnd]                ; CON as handle 1
        mov     cx,1
        mov     ah,46h
        int     21h
        mov     ax,KEY_CTRLP
        call    kb_stuff
        mov     ah,0Bh                  ; the echo on
        int     21h
        mov     dx,OFFSET m_echoln
        call    puts
        mov     ax,KEY_CTRLP
        call    kb_stuff
        mov     ah,0Bh                  ; and off
        int     21h
        mov     bx,[r4]                 ; handle 1 back
        mov     cx,1
        mov     ah,46h
        int     21h
        mov     bx,[r4]
        mov     ah,3Eh
        int     21h
te_close:
        call    close_h
te_out:
        call    kb_flush
        mov     dx,OFFSET m_echo
        jmp     puts

;=====================================================================
; The handlers: count, and carry on - and my1b passes the call on when
; chain1b says so.
;=====================================================================
my23:
        inc     word ptr cs:[ncc]
        iret

my1b:
        inc     word ptr cs:[n1b]
        cmp     byte ptr cs:[chain1b],0
        je      m1b_out
        jmp     dword ptr cs:[old1b]
m1b_out:
        iret

my9:                                    ; a resident program's keyboard
        jmp     dword ptr cs:[old9]     ; handler, with nothing to do

;=====================================================================
; Helpers
;=====================================================================

; set_brk - DL = BREAK, through 3301h
set_brk:
        mov     ax,3301h
        int     21h
        ret

; arm_cc - DL = BREAK; the buffer emptied, the count zeroed, and one
;          ^C put in the buffer
arm_cc:
        call    set_brk
        call    kb_flush
        mov     word ptr [ncc],0
        mov     ax,KEY_CTRLC
        jmp     kb_stuff

; front - AX = the key at the front of the buffer, or -1 if none
front:
        mov     ah,01h
        int     16h
        jnz     fr_out
        mov     ax,-1
fr_out:
        ret

; nwait - AL = how many keys are in the buffer
nwait:
        push    es
        push    40h
        pop     es
        mov     ax,es:[1Ch]             ; tail - head, in words, round the
        sub     ax,es:[1Ah]             ; ring
        jns     nw_pos
        add     ax,es:[82h]
        sub     ax,es:[80h]
nw_pos:
        shr     ax,1
        pop     es
        ret

; kb_flush - the BIOS's buffer emptied, and DOS's mark of a Ctrl-Break
;            taken with it: 0Ch flushes as MS-DOS's does, without
;            looking for a ^C
kb_flush:
        mov     ah,01h
        int     16h
        jz      kf_dos
        mov     ah,00h
        int     16h
        jmp     short kb_flush
kf_dos:
        mov     ax,0C00h
        int     21h
        ret

; kb_stuff - AX = a key, into the BIOS keyboard buffer
kb_stuff:
        mov     cx,ax
        mov     ah,05h
        int     16h
        ret

; press_cbrk - Ctrl-Break, as a 101-key keyboard sends it: Ctrl down,
;              E0 46 and E0 C6 for the Pause key, Ctrl up
press_cbrk:
        mov     si,OFFSET cbrk_scans
pc_lp:
        lodsb
        or      al,al
        jz      pc_out
        mov     dl,al
        mov     ax,0F123h
        int     21h
        jmp     short pc_lp
pc_out:
        ret

; open_w - DX = a device name: opened for writing into [hnd]
open_w:
        mov     ax,3D01h
        int     21h
        jnc     ow_ok
        mov     ax,-1
ow_ok:
        mov     [hnd],ax
        ret

close_h:
        mov     bx,[hnd]
        cmp     bx,-1
        je      ch_out
        mov     ah,3Eh
        int     21h
ch_out:
        ret

; write64 - sixty-four dots through 40h to [hnd]
write64:
        mov     bx,[hnd]
        mov     cx,64
        mov     dx,OFFSET dots
        mov     ah,40h
        int     21h
        ret

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

puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
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
ncc         dw  0                       ; INT 23h calls
nfail       dw  0
hnd         dw  -1
r1          dw  0
r2          db  0
r3          db  0
r4          dw  0
was_brk     db  0
was_71      db  0
old23       dd  0
old1b       dd  0
old9        dd  0
n1b         dw  0                       ; INT 1Bh calls
msgp        dw  0
chain1b     db  0                       ; my1b passes the call on

cbrk_scans  db  1Dh, 0E0h, 46h, 0E0h, 0C6h, 9Dh, 0
nm_nul      db  'NUL',0
nm_con      db  'CON',0
dots        db  64 dup('.')

msg_hi      db  'BRKTEST: BREAK, and the ^C nobody is reading',13,10,'$'
msg_wasoff  db  'BREAK was off when it started',13,10,'$'
msg_wason   db  'BREAK was on when it started',13,10,'$'
msg_end     db  'BRKTEST: $'
msg_failed  db  ' failed',13,10,'$'
msg_crlf    db  13,10,'$'
m_ok        db  '  ok    $'
m_fail      db  '  FAIL  $'

m_setget    db  '3301h sets BREAK, 3300h reads it, 3302h swaps it',13,10,'$'
m_offdisk   db  'BREAK OFF: 19h leaves a waiting ^C where it is',13,10,'$'
m_off0b     db  'BREAK OFF: 0Bh takes it, and the call is made again',13,10,'$'
m_ondisk    db  'BREAK ON: 19h takes it',13,10,'$'
m_behind    db  'BREAK ON: a ^C behind a key waits until the key is read',13,10,'$'
m_cbreak    db  'Ctrl-Break empties the buffer, sets 40:71 bit 7, leaves 0000h',13,10,'$'
m_cbon      db  'BREAK ON: 19h takes a Ctrl-Break typed behind a key',13,10,'$'
m_cbread    db  '07h reads Ctrl-Break as 03h',13,10,'$'
m_off02     db  'BREAK OFF: 64 characters through 02h take a ^C',13,10,'$'
m_offnul    db  'BREAK OFF: 64 bytes through 40h to NUL do not',13,10,'$'
m_onnul     db  'BREAK ON: 40h to NUL takes it',13,10,'$'
m_offcon    db  'BREAK OFF: 64 bytes through 40h to CON take it',13,10,'$'
m_offraw    db  'BREAK OFF: ...but not with CON in raw mode',13,10,'$'
m_on33      db  'BREAK ON: 33h is not broken into',13,10,'$'
m_own1b     db  "A program's own INT 1Bh is called for Ctrl-Break, and keeps it",13,10,'$'
m_pass1b    db  "...and one that passes the call on gets DOS's ^C",13,10,'$'
m_rom1b     db  "With INT 09h hooked, the BIOS's handler calls DOS's INT 1Bh",13,10,'$'
m_stop      db  '^S at 0Bh: taken, with the key that ends the wait',13,10,'$'
m_stopc     db  '^S and then ^C: the wait ends in a break',13,10,'$'
m_stop02    db  '64 characters through 02h stop for ^S',13,10,'$'
m_stop08    db  "08h: ^S and the key after it are DOS's",13,10,'$'
m_raws      db  '07h and 06h read ^S as a key',13,10,'$'
m_ctlp      db  '^P at 0Bh is taken, for the printer echo',13,10,'$'
m_prtsc     db  'Ctrl+PrtSc is ^P',13,10,'$'
m_altr      db  "Alt+R's scan code is a character to 08h, not ^S",13,10,'$'
m_echo      db  '  --    ^P: one line sent to the screen with the printer echo on',13,10,'$'
m_echoln    db  'BRKTEST: this line was echoed to the printer by ^P',13,10,'$'

_BT     ENDS
        END     start
