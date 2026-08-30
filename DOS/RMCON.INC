;=====================================================================
; RMCON.INC - the console, and the character calls that go through it
;
; Everything here is the BIOS underneath: INT 10h AH=0Eh out, INT 16h
; in.  That is not a shortcut, it is the only choice - while Windows
; owns the machine it owns the video hardware too, and a console that
; wrote screen memory directly would be writing into whatever mode
; Windows has the adapter in.  The BIOS is the one path that is still
; the BIOS afterwards.
;
; TAB IS EXPANDED HERE AND NOWHERE ELSE.  AH=0Eh does carriage return,
; line feed, backspace and bell and does NOT do tab - it prints a
; graphic character for it.  DOS's CON device expands tabs to the next
; eight-column stop, and a program printing a columned listing through
; 09h is relying on that.
;=====================================================================

;---------------------------------------------------------------------
; con_out - DL = the character, out to the console.
;---------------------------------------------------------------------
con_out:
        push    ax
        push    bx
        push    cx
        push    dx

        cmp     dl,9
        jne     co_one

        ; A tab is however many spaces reach the next eight-column
        ; stop, and where that is has to be asked of the BIOS rather
        ; than counted here: this is not the only thing writing to the
        ; screen and a private column counter would drift from it the
        ; first time anything else printed.
        mov     ah,03h
        xor     bh,bh
        int     10h                     ; DL = column
        mov     al,dl
        and     al,7
        mov     cl,8
        sub     cl,al
        movzx   cx,cl
        mov     dl,' '
co_tab:
        push    cx
        call    co_raw
        pop     cx
        loop    co_tab
        jmp     short co_done

co_one:
        call    co_raw
co_done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

co_raw:
        push    ax
        push    bx
        mov     al,dl
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
; con_in - a character from the keyboard into AL, waiting for one.
;
; An extended key arrives from the BIOS as AL=0 with the scan code in
; AH, and DOS hands that to a program as TWO calls: a zero, and then
; the scan code.  The pending byte below is that second call, and it is
; why every path in has to come through here.
;---------------------------------------------------------------------
con_in:
        push    bx
        cmp     byte ptr [con_pend],0
        je      ci_fresh
        mov     al,[con_pend]
        mov     byte ptr [con_pend],0
        pop     bx
        ret
ci_fresh:
        xor     ah,ah
        int     16h
        or      al,al
        jnz     ci_have
        mov     [con_pend],ah           ; the scan code, next time round
        xor     al,al
ci_have:
        pop     bx
        ret

;---------------------------------------------------------------------
; con_stat - ZF clear if a character is waiting.  AL = it, unconsumed.
;---------------------------------------------------------------------
con_stat:
        cmp     byte ptr [con_pend],0
        jne     cs_yes
        push    bx
        mov     ah,01h
        int     16h
        pop     bx
        jz      cs_no
        or      al,al                   ; an extended key still counts as
        jnz     cs_yes                  ; a keystroke waiting
cs_yes:
        or      sp,sp                   ; ZF = 0, and nothing else touched
        ret
cs_no:
        cmp     sp,sp                   ; ZF = 1
        ret

;---------------------------------------------------------------------
; con_break - AL = a character just read.  If it is Ctrl-C and
;             checking is on, run the program's INT 23h.
;
; INT 23h RETURNING is the interesting case and the one that gets
; forgotten: a handler may simply IRET, which means "carry on", and
; the call that was interrupted has to finish normally.  Only a
; handler that terminates does not come back here.
;---------------------------------------------------------------------
con_break:
        cmp     al,3
        jne     cb_no
        push    ax
        mov     dl,'^'
        call    con_out
        mov     dl,'C'
        call    con_out
        mov     dl,13
        call    con_out
        mov     dl,10
        call    con_out
        int     23h
        pop     ax
cb_no:
        ret

;=====================================================================
; 00h - terminate.  The DOS 1 call, and CS is the PSP.
;=====================================================================
fn00:
        mov     word ptr [bp+F_AX],4C00h
        jmp     fn4C

;=====================================================================
; 01h - read a character with echo
;=====================================================================
fn01:
        call    con_in
        call    con_break
        push    ax
        mov     dl,al
        cmp     al,0                    ; the second half of an extended key
        je      f01_noecho              ; is not a printable thing
        call    con_out
f01_noecho:
        pop     ax
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; 02h - write the character in DL
;=====================================================================
fn02:
        mov     dl,byte ptr [bp+F_DX]
        call    con_out
        mov     byte ptr [bp+F_AX],dl
        jmp     i21_ok

;=====================================================================
; 06h - direct console I/O.  DL = FFh reads, anything else writes.
;
; The one console call with NO Ctrl-C checking, which is the whole
; reason it exists: an editor wants the keystroke, not an interrupt.
;=====================================================================
fn06:
        mov     dl,byte ptr [bp+F_DX]
        cmp     dl,0FFh
        je      f06_in
        call    con_out
        mov     byte ptr [bp+F_AX],dl
        jmp     i21_ok
f06_in:
        call    con_stat
        jz      f06_none
        call    con_in
        mov     byte ptr [bp+F_AX],al
        and     word ptr [bp+F_FL],NOT FL_ZF
        jmp     i21_ok
f06_none:
        mov     byte ptr [bp+F_AX],0
        or      word ptr [bp+F_FL],FL_ZF
        jmp     i21_ok

;=====================================================================
; 07h - character input, no echo, no Ctrl-C check
; 08h - character input, no echo, with the check
;=====================================================================
fn07:
        call    con_in
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

fn08:
        call    con_in
        call    con_break
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; 09h - write the $-terminated string at DS:DX
;=====================================================================
fn09:
        push    ds
        mov     ds,[bp+F_DS]
        mov     si,[bp+F_DX]
f09_loop:
        lodsb
        cmp     al,'$'
        je      f09_done
        mov     dl,al
        push    ds
        push    si
        mov     ax,cs
        mov     ds,ax
        call    con_out
        pop     si
        pop     ds
        jmp     short f09_loop
f09_done:
        pop     ds
        mov     byte ptr [bp+F_AX],'$'
        jmp     i21_ok

;=====================================================================
; 0Ah - buffered input at DS:DX.  [0] = room, [1] = how much was read,
;       [2..] = the text, ending in a carriage return.
;
; BACKSPACE HAS TO ERASE AND NOT JUST MOVE.  The BIOS does the moving;
; the space and the second backspace are DOS's, and a program that
; edits its input line looks wrong without them.
;=====================================================================
fn0A:
        push    ds
        mov     ds,[bp+F_DS]
        mov     bx,[bp+F_DX]
        mov     cl,[bx]                 ; room, including the return
        xor     ch,ch
        or      cl,cl
        jz      f0A_done
        dec     cx                      ; the return needs a place too
        xor     si,si                   ; how many so far
f0A_key:
        push    ds
        push    bx
        push    cx
        push    si
        mov     ax,cs
        mov     ds,ax
        call    con_in
        call    con_break
        pop     si
        pop     cx
        pop     bx
        pop     ds

        cmp     al,13
        je      f0A_end
        cmp     al,8
        je      f0A_bs
        or      al,al                   ; extended keys are not text
        jz      f0A_key
        cmp     si,cx
        jae     f0A_full
        mov     [bx+si+2],al
        inc     si
        push    ds
        push    bx
        push    cx
        push    si
        mov     dl,al
        mov     ax,cs
        mov     ds,ax
        call    con_out
        pop     si
        pop     cx
        pop     bx
        pop     ds
        jmp     short f0A_key
f0A_full:
        push    ds
        push    bx
        push    cx
        push    si
        mov     ax,cs
        mov     ds,ax
        mov     dl,7                    ; the bell, which is what DOS did
        call    con_out
        pop     si
        pop     cx
        pop     bx
        pop     ds
        jmp     short f0A_key
f0A_bs:
        or      si,si
        jz      f0A_key
        dec     si
        push    ds
        push    bx
        push    cx
        push    si
        mov     ax,cs
        mov     ds,ax
        mov     dl,8
        call    con_out
        mov     dl,' '
        call    con_out
        mov     dl,8
        call    con_out
        pop     si
        pop     cx
        pop     bx
        pop     ds
        jmp     f0A_key
f0A_end:
        mov     byte ptr [bx+si+2],13
        mov     ax,si                   ; how many characters, into
        mov     [bx+1],al               ; the count byte DOS puts there
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     dl,13
        call    con_out
        mov     dl,10
        call    con_out
        pop     ds
f0A_done:
        pop     ds
        jmp     i21_ok

;=====================================================================
; 0Bh - is there a keystroke waiting?  AL = FFh yes, 00h no.
;=====================================================================
fn0B:
        call    con_stat
        jz      f0B_none
        mov     byte ptr [bp+F_AX],0FFh
        jmp     i21_ok
f0B_none:
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok

;=====================================================================
; 0Ch - throw away what is typed ahead, then do the input call in AL.
;
; Only 01h, 06h, 07h, 08h and 0Ah may follow; anything else is a flush
; and nothing more, which is what DOS did and what programs rely on to
; clear the buffer without reading.
;=====================================================================
fn0C:
        mov     ah,01h                  ; drain the BIOS buffer
f0C_drain:
        push    bx
        mov     ah,01h
        int     16h
        pop     bx
        jz      f0C_dry
        push    bx
        xor     ah,ah
        int     16h
        pop     bx
        jmp     short f0C_drain
f0C_dry:
        mov     byte ptr [con_pend],0

        mov     al,byte ptr [bp+F_AX]
        cmp     al,01h
        je      f0C_go
        cmp     al,06h
        je      f0C_go
        cmp     al,07h
        je      f0C_go
        cmp     al,08h
        je      f0C_go
        cmp     al,0Ah
        je      f0C_go
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok
f0C_go:
        movzx   bx,al
        shl     bx,1
        mov     byte ptr [bp+F_AX+1],al ; the frame has to say which call it
        jmp     word ptr [i21_tab + bx] ; became, for the ones that look

;=====================================================================
; Data
;=====================================================================
con_pend    db  0                       ; the second byte of an extended key
