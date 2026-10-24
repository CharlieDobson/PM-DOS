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
        ; A LOADED CON - ANSI.SYS - takes it instead, when there is one
        ; (drv_coninit, RMDEV.INC).
        cmp     word ptr cs:[rd_condrv+2],0
        jne     co_drv
        push    ax
        push    bx
        mov     al,dl
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        pop     bx
        pop     ax
        ret
co_drv:
        jmp     drv_putc

;---------------------------------------------------------------------
; con_in - a character from the keyboard into AL, waiting for one.
;
; An extended key arrives from the BIOS as AL=0 with the scan code in
; AH, and DOS hands that to a program as TWO calls: a zero, and then
; the scan code.  The pending byte below is that second call, and it is
; why every path in has to come through here.
;
; THE BREAK KEY IS A MARK AND NOT A KEY.  INT 1Bh is still the kernel's
; handler down here (GNLS_BRKSTUB, PMDOS.INC), and like IO.SYS's it
; marks the key and no more (RL_BRKFLAG): that mark is the ^C, read
; before anything in the BIOS's buffer, and the 0000h the BIOS leaves
; behind it is skipped - so a program whose own INT 1Bh did not pass
; the call on gets no ^C, as under MS-DOS (mschar.asm, chrin).  And
; Ctrl+PrtSc, 7200h, is ^P.
;---------------------------------------------------------------------
con_in:
        push    bx
        cmp     byte ptr [con_pend],0
        je      ci_wait
        mov     al,[con_pend]
        mov     byte ptr [con_pend],0
        pop     bx
        ret
ci_wait:
        call    con_marked
        jnz     ci_break
        mov     ah,01h
        int     16h
        jnz     ci_take
        call    con_idle
        jmp     short ci_wait
ci_take:
        xor     ah,ah
        int     16h
        or      ax,ax                   ; the Break key's leftover
        jz      ci_wait
        cmp     ax,7200h
        je      ci_prtsc
        or      al,al
        jnz     ci_have
        mov     [con_pend],ah           ; the scan code, next time round
        xor     al,al
        jmp     short ci_have
ci_prtsc:
        mov     al,'P' - '@'
        jmp     short ci_have
ci_break:
        call    con_unmark
        mov     al,3
ci_have:
        pop     bx
        ret

;---------------------------------------------------------------------
; con_idle - one pass of waiting for a key.
;
; WAIT BY SAYING SO.  A DOS with nothing to do calls INT 28h, which is
; where a TSR gets its turn and, under Windows, where DOSMGR lets another
; virtual machine run while this one waits for a key.  That other
; machine may call this DOS, so the process state is published before
; the call and read back after it - see sda_sync in RMINFO.INC.  Without
; the INT 28h a DOS box waiting at a prompt holds InDOS at one and
; Windows waits with it.
;---------------------------------------------------------------------
con_idle:
        call    sda_sync
        call    crit_out                ; another machine may have DOS
        int     28h                     ; while this one waits
        call    crit_in
        call    sda_pull
        ret

;---------------------------------------------------------------------
; con_marked - ZF clear if the Break key's mark is waiting (con_in).
;              Flags only.
; con_unmark - take the mark.  All registers and flags preserved.
;---------------------------------------------------------------------
con_marked:
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        cmp     byte ptr es:[RL_BRKFLAG],0
        pop     es
        ret
con_unmark:
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     byte ptr es:[RL_BRKFLAG],0
        pop     es
        ret

;---------------------------------------------------------------------
; con_stat - ZF clear if a character is waiting.  AL = it, unconsumed,
;            as con_in will read it: 03h for the Break key's mark, 10h
;            for Ctrl+PrtSc.  The Break key's 0000h is taken out of the
;            way and the buffer looked at again, as MS-DOS's CON does
;            (mschar.asm, con_rdnd).
;---------------------------------------------------------------------
con_stat:
        cmp     byte ptr [con_pend],0
        jne     cs_yes
        mov     al,3
        call    con_marked
        jnz     cs_yes
        push    bx
cs_again:
        mov     ah,01h
        int     16h
        jz      cs_none
        or      ax,ax
        jnz     cs_key
        xor     ah,ah                   ; a Break's 0000h: gone
        int     16h
        jmp     short cs_again
cs_key:
        pop     bx
        cmp     ax,7200h
        jne     cs_yes
        mov     al,'P' - '@'
cs_yes:
        or      sp,sp                   ; ZF = 0, and nothing else touched
        ret
cs_none:
        pop     bx
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
        call    con_outp
        mov     dl,'C'
        call    con_outp
        mov     dl,13
        call    con_outp
        mov     dl,10
        call    con_outp
        int     23h
        pop     ax
cb_no:
        ret

;---------------------------------------------------------------------
; con_chkc - a ^C at the front of the BIOS's buffer is taken, and
;            con_break runs the program's INT 23h.  Anything else
;            waiting is left there.  All registers preserved.
;
; The kernel's brk_check, and MS-DOS's DSKSTATCHK: only at the
; outermost level, the keyboard and not standard input, only the key at
; the front - and the Break key's mark even with half an extended key
; owed.
;---------------------------------------------------------------------
con_chkc:
        cmp     byte ptr [rd_indos],1
        jne     ckc_out
        push    ax
        push    bx
        push    dx
        call    con_marked
        jnz     ckc_take                ; the Break key
        cmp     byte ptr [con_pend],0
        jne     ckc_no                  ; a scan code is owed first
        mov     ah,01h
        int     16h
        jz      ckc_no
        cmp     al,3
        jne     ckc_no
ckc_take:
        mov     byte ptr [con_pend],0
        call    con_in                  ; the mark or the key: 03h either way
        call    con_break
ckc_no:
        pop     dx
        pop     bx
        pop     ax
ckc_out:
        ret

;---------------------------------------------------------------------
; con_statchk - the kernel's stat_check, MS-DOS's STATCHK: con_chkc,
;               and then ^S or ^P at the front of the buffer, either of
;               which is taken.  ^S holds everything until another key
;               is pressed, and that key is taken too unless it is ^P
;               (the echo turns over) or a ^C (a break); ^P turns the
;               printer echo on or off (con_echo).  CF set if a key was
;               taken here.  All registers preserved.
;
; The keyboard is what RMDOS's console calls read, whatever standard
; input is, and so it is what this looks at.
;---------------------------------------------------------------------
con_statchk:
        call    con_chkc
        cmp     byte ptr [con_pend],0
        jne     csk_no                  ; Alt+Q's and Alt+R's scan codes
        push    ax                      ; are ^P's and ^S's
        push    dx
        call    con_stat
        jz      csk_none
        cmp     al,'S' - '@'
        je      csk_stop
        cmp     al,'P' - '@'
        jne     csk_none
        call    con_in                  ; the ^P
        cmp     al,3
        je      csk_go                  ; ...or the Break key, which got
        not     byte ptr [rd_pflag]     ; there first (stat_check says how)
        jmp     short csk_took
csk_stop:
        call    con_in                  ; the ^S
        cmp     al,3
        je      csk_go                  ; ...or the Break key
        call    con_in                  ; and the key that ends the wait,
        mov     byte ptr [con_pend],0   ; taken whole
        cmp     al,'P' - '@'
        jne     csk_go
        not     byte ptr [rd_pflag]
        jmp     short csk_took
csk_go:
        cmp     byte ptr [rd_indos],1   ; con_chkc's rules for a ^C
        jne     csk_took
        call    con_break
csk_took:
        pop     dx
        pop     ax
        stc
        ret
csk_none:
        pop     dx
        pop     ax
csk_no:
        clc
        ret

;---------------------------------------------------------------------
; con_inck - con_in for 01h, 08h, 0Ah and a read of CON in cooked mode,
;            which leave ^S, ^P and ^C at the front to con_statchk
;            while they wait, as MS-DOS's 08h does.  06h and 07h read
;            them as keys.
;---------------------------------------------------------------------
con_inck:
        cmp     byte ptr [con_pend],0
        jne     con_in
cik_wait:
        call    con_stat
        jnz     cik_key
        call    con_idle
        jmp     short cik_wait
cik_key:
        call    con_statchk
        jc      cik_wait                ; that one was DOS's
        call    con_stat
        jz      cik_wait                ; ...and so was a ^C con_chkc took
        jmp     con_in

;---------------------------------------------------------------------
; con_outchk - CX more characters are going to the screen through 02h,
;              09h or a write to CON.  Each 64th looks at the keyboard
;              (con_statchk), BREAK ON or OFF, as MS-DOS's OUTT does: it
;              is how ^C stops DIR and TYPE with BREAK OFF, and ^S holds
;              them.  All registers preserved.
;---------------------------------------------------------------------
con_outchk:
        push    ax
        mov     ax,[rd_charco]
        add     [rd_charco],cx
        xor     ax,[rd_charco]
        and     ax,0FFC0h               ; a multiple of 64 crossed
        pop     ax
        jz      coc_out
        jmp     con_statchk
coc_out:
        ret

;---------------------------------------------------------------------
; con_outp  - DL to the screen (con_out), and to the printer as well
;             while ^P has the echo on.  All registers preserved.
; con_echo  - the printer half alone, for DL just written to the screen
;             through 01h, 02h, 09h, 0Ah, or a write to CON in cooked
;             mode.
; con_echos - the same for CX bytes at ES:DI.
;
; As the kernel's pe_char: only while standard output is a device, to
; LPT1 through INT 17h, and a printer that does not take the byte turns
; the echo off.  RMDOS's own PRN goes nowhere, so the echo goes to the
; BIOS and not through it.
;---------------------------------------------------------------------
con_outp:
        call    con_out
con_echo:
        cmp     byte ptr cs:[rd_pflag],0
        jne     ce_on
        ret
ce_on:
        pushad
        push    ds
        push    es
        push    cs
        pop     ds
        mov     bx,1
        call    sft_get                 ; -> SI; DX survives it
        jc      ce_dev                  ; no handle 1: the console
        test    byte ptr [si+2Bh],01h   ; SFT_FLAGS and SFF_DEVICE, as in
        jz      ce_out                  ; con_write
ce_dev:
        mov     ax,40h
        mov     es,ax
        cmp     word ptr es:[08h],0     ; LPT1's port
        je      ce_off
        mov     al,dl
        xor     ah,ah                   ; AH = 00h: print AL
        xor     dx,dx
        int     17h
        test    ah,29h                  ; time-out, I/O error, no paper
        jz      ce_out
ce_off:
        mov     byte ptr [rd_pflag],0
ce_out:
        pop     es
        pop     ds
        popad
        ret

con_echos:
        cmp     byte ptr cs:[rd_pflag],0
        je      ces_out
        jcxz    ces_out
        push    cx
        push    dx
        push    di
ces_lp:
        mov     dl,es:[di]
        call    con_echo
        inc     di
        loop    ces_lp
        pop     di
        pop     dx
        pop     cx
ces_out:
        ret

;---------------------------------------------------------------------
; rd_brkcall - BREAK ON: the call in the frame looks for a ^C before it
;              starts.  The calls left out are the kernel's (brk_call in
;              INT21.INC says why).  All registers preserved.
;---------------------------------------------------------------------
rd_brkcall:
        cmp     byte ptr [rd_brkchk],0
        je      rbc_out
        push    ax
        mov     al,byte ptr [bp+F_AX+1]
        cmp     al,0Ch
        jbe     rbc_skip
        cmp     al,33h
        je      rbc_skip
        cmp     al,4Ch
        je      rbc_skip
        cmp     al,50h
        je      rbc_skip
        cmp     al,51h
        je      rbc_skip
        cmp     al,59h
        je      rbc_skip
        cmp     al,62h
        je      rbc_skip
        call    con_chkc
rbc_skip:
        pop     ax
rbc_out:
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
        call    con_inck
        call    con_break
        push    ax
        mov     dl,al
        cmp     al,0                    ; the second half of an extended key
        je      f01_noecho              ; is not a printable thing
        call    con_outp
f01_noecho:
        pop     ax
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; con_write - ES:DI = bytes, ECX = how many, out through HANDLE 1 of
;             the running program, which is where DOS sends 02h, 06h
;             and 09h.  Redirected, they go to the file; otherwise to
;             the console, through dev_write and so through con_out
;             and its tab expansion.  A program with no handle 1 at
;             all gets the console.
;
; Until v0.63 the three calls went straight to con_out, and a program
; in a Windows DOS box whose output was sent to a file - SDSTAT >>
; CACHEBOX.TXT, the first one to try - printed on the screen instead
; and left the file empty.  COMMAND.COM's own ECHO and COPY went
; through 40h and were fine, which is why nothing had noticed.
;=====================================================================
con_write:
        pushad
        push    ds
        push    es
        or      ecx,ecx
        jz      cw_out
        mov     bx,1
        call    sft_get                 ; -> SI, the handle's file
        jc      cw_console
        call    sft_isnet               ; a redirector's file (RMNET.INC)?
        je      cw_net
        test    byte ptr [si+2Bh],01h    ; SFT_FLAGS and SFF_DEVICE, which
        jnz     cw_dev                  ; RMFILE.INC defines after this file
                                        ; and wasm will not take early
        call    sft_touch
        mov     byte ptr [rd_iowr],1
        call    file_io
        jmp     short cw_out
cw_dev:
        call    dev_write
        jmp     short cw_out
cw_net:
        mov     ax,1109h
        call    net_xfer                ; ES:DI and CX are the bytes already
        jmp     short cw_out
cw_console:
        push    es
        pop     ds
        mov     si,di
cw_loop:
        lodsb
        mov     dl,al
        push    ecx
        push    si
        push    ds
        mov     ax,cs
        mov     ds,ax
        call    con_out
        pop     ds
        pop     si
        pop     ecx
        dec     ecx
        jnz     cw_loop
cw_out:
        pop     es
        pop     ds
        popad
        ret

;=====================================================================
; 02h - write the character in DL
;=====================================================================
fn02:
        mov     cx,1                    ; every 64th looks at the keyboard
        call    con_outchk
        mov     dl,byte ptr [bp+F_DX]
        mov     [rd_conch],dl
        push    cs
        pop     es
        mov     di,OFFSET rd_conch
        mov     ecx,1
        call    con_write
        call    con_echo                ; and ^P prints it as well
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
        mov     [rd_conch],dl
        push    cs
        pop     es
        mov     di,OFFSET rd_conch
        mov     ecx,1
        call    con_write
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
        call    con_inck
        call    con_break
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; 09h - write the $-terminated string at DS:DX
;=====================================================================
fn09:
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        xor     ecx,ecx                 ; up to the dollar, and all of it
f09_len:                                ; in one write
        cmp     byte ptr es:[di],'$'
        je      f09_have
        inc     di
        inc     cx
        jnz     f09_len                 ; 64K without one: enough
f09_have:
        call    con_outchk              ; the keyboard every 64th, as 02h
        mov     di,[bp+F_DX]
        call    con_write
        call    con_echos
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
        call    con_inck
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
        call    con_outp
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
        call    con_outp
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
        call    con_outp
        mov     dl,' '
        call    con_outp
        mov     dl,8
        call    con_outp
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
        call    con_outp
        mov     dl,10
        call    con_outp
        pop     ds
f0A_done:
        pop     ds
        jmp     i21_ok

;=====================================================================
; 0Bh - is there a keystroke waiting?  AL = FFh yes, 00h no.  A ^C, ^S
;       or ^P is acted on first, BREAK ON or OFF, as MS-DOS does.
;=====================================================================
fn0B:
        call    con_statchk
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
        call    con_unmark              ; and the Break key's ^C, as MS-DOS's
                                        ; CON's flush clears ALTAH

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
rd_conch    db  0                       ; the one byte 02h and 06h write
rd_pflag    db  0                       ; ^P: FFh while the printer echoes
                                        ; the screen (con_echo)
