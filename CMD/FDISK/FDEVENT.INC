;=====================================================================
; FDEVENT.INC - the keyboard, the mouse, and the key file that stands
;               in for both in a test.  Included by FDTUI.INC.
;
; ev_get waits for the next thing to happen and says what it was:
;
;   EV_KEY      ev_kcode = the key: AL the character, AH the scan code,
;               AL = 0 for a grey key
;   EV_DOWN     the left button pressed, on ev_row, ev_col
;   EV_UP       let go
;   EV_DRAG     moved to another cell with the button held
;   EV_DOUBLE   pressed again on the same cell soon after
;   EV_EOF      the key file ran out
;
; THE MOUSE IS READ AS EDIT READS IT (CMD\EDIT\SCR.C).  21h/F0h AL=8 is
; asked to wake for the mouse as well as for a key, and each wake is a
; look at INT 33h: 05h and 06h for the presses and releases since the
; last look, which a quick click would otherwise lose between two
; looks, and 03h for where the pointer is now.  What they say becomes
; events on screen cells, queued behind the keys.
;
; THE KEY FILE (FDTEST=path, input redirected): a byte a key, 0 and a
; scan code for a grey one, FFh FFh for a picture of the screen written
; to the path, and FEh, the buttons, a row and a column for the mouse -
; which is put on that cell and given those buttons through the driver
; (INT 33h 04h, 21h/F0h AL=0Bh), so what the program reads back is
; what the mouse itself would have said.  TOOLS\MKFDKEYS.PL writes
; them.  A file that runs out is EV_EOF from then on, which every loop
; takes as Esc - so a short script ends the program and cannot hang it.
;=====================================================================

;--- ev_init - the mouse, if there is one ----------------------------
ev_init:
        pushad
        mov     byte ptr [ms_on],0
        mov     eax,0F003h              ; the screen: DH rows, DL columns
        int     21h
        movzx   eax,dh
        or      eax,eax
        jnz     evi_rows
        mov     eax,TUI_ROWS
evi_rows:
        mov     [ms_rows],eax
        movzx   eax,dl
        or      eax,eax
        jnz     evi_cols
        mov     eax,TUI_COLS
evi_cols:
        mov     [ms_cols],eax
        xor     eax,eax                 ; reset: AX = FFFFh when one is there
        int     33h
        cmp     ax,0FFFFh
        jne     evi_out
        mov     byte ptr [ms_on],1
        mov     eax,26h                 ; the furthest it will say it is
        int     33h
        cmp     ecx,1
        jl      evi_xdef
        cmp     edx,1
        jge     evi_ext
evi_xdef:
        mov     ecx,639
        mov     edx,199
evi_ext:
        mov     [ms_xmax],ecx
        mov     [ms_ymax],edx
        mov     eax,3
        int     33h
        and     ebx,1
        mov     [ms_down],ebx
        mov     eax,5                   ; and nothing from before
        xor     ebx,ebx
        int     33h
        mov     eax,6
        xor     ebx,ebx
        int     33h
        mov     eax,1                   ; the pointer on
        int     33h
evi_out:
        popad
        ret

;--- ev_done - the mouse put back as it was found --------------------
ev_done:
        cmp     byte ptr [ms_on],0
        je      evd_out
        push    eax
        xor     eax,eax                 ; a reset, which hides the pointer
        int     33h
        pop     eax
        mov     byte ptr [ms_on],0
evd_out:
        ret

;=====================================================================
; ev_get - the next event.  -> EAX = EV_*.
;=====================================================================
ev_get:
        push    ebx
        push    ecx
        push    edx
evg_again:
        call    mq_pop                  ; a mouse event waiting goes first
        or      eax,eax
        jnz     evg_out
        cmp     byte ptr [kt_on],0
        jne     evg_file
        movzx   edx,byte ptr [ev_shift]
        cmp     byte ptr [ms_on],0
        je      evg_wait
        or      edx,0200h               ; DH bit 1: the mouse wakes it too
evg_wait:
        mov     eax,0F008h
        int     21h
        and     bl,SHIFT_KEYS
        mov     [ev_shift],bl
        cmp     al,1
        je      evg_key
        cmp     al,2
        jne     evg_again               ; only the shift keys changed
        call    ms_poll
        jmp     short evg_again
evg_key:
        mov     eax,0F007h
        int     21h
        mov     [ev_kcode],ax
        mov     eax,EV_KEY
evg_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret
evg_file:
        call    kt_next                 ; -> an event, or 0 to look again
        or      eax,eax
        jz      evg_again
        jmp     short evg_out

;--- kt_next - one record of the key file -> EAX = an event, or 0 ----
kt_next:
        call    tk_byte
        jc      ktn_eof
        cmp     al,0FEh
        je      ktn_mouse
        cmp     al,0FFh
        je      ktn_ff
        mov     byte ptr [ms_click],0   ; a key in between: no double click
        or      al,al
        jz      ktn_grey
        xor     ah,ah
        jmp     short ktn_key
ktn_grey:
        call    tk_byte
        jc      ktn_eof
        mov     ah,al
        xor     al,al
ktn_key:
        mov     [ev_kcode],ax
        mov     eax,EV_KEY
        ret
ktn_ff:
        call    tk_byte
        jc      ktn_eof
        cmp     al,0FFh
        jne     ktn_none                ; a shift state: nothing here uses it
        call    tui_dump
ktn_none:
        xor     eax,eax
        ret
ktn_mouse:
        call    tk_byte
        jc      ktn_eof
        mov     [kt_btn],al
        call    tk_byte
        jc      ktn_eof
        mov     [kt_row],al
        call    tk_byte
        jc      ktn_eof
        mov     [kt_col],al
        call    ms_test
        xor     eax,eax                 ; whatever it queued is read next
        ret
ktn_eof:
        cmp     byte ptr [kt_hold],0
        jne     ktn_hold
        mov     eax,EV_EOF
        ret
ktn_hold:
        mov     byte ptr [kt_on],0      ; the keyboard from here on
        xor     eax,eax
        ret

;---------------------------------------------------------------------
; ms_test - the mouse, as the key file says: onto kt_row, kt_col with
;           the buttons kt_btn, then read back like any other movement.
;           A release where the pointer already was is a click, and a
;           press straight after it on the same cell is a double one -
;           in a test there is no clock, only what comes next.
;---------------------------------------------------------------------
ms_test:
        pushad
        cmp     byte ptr [ms_on],0
        je      mtt_out
        mov     eax,[ms_down]
        mov     [mt_wasdown],eax
        mov     eax,[ms_row]
        mov     [mt_prow],eax
        mov     eax,[ms_col]
        mov     [mt_pcol],eax
        ; the pointer onto the cell: 04h, in the driver's own units
        movzx   eax,byte ptr [kt_col]
        mov     ecx,[ms_xmax]
        inc     ecx
        mul     ecx
        div     dword ptr [ms_cols]
        mov     ecx,eax
        movzx   eax,byte ptr [kt_row]
        mov     ebx,[ms_ymax]
        inc     ebx
        mul     ebx
        div     dword ptr [ms_rows]
        mov     edx,eax
        mov     eax,4
        int     33h
        ; and the buttons, as if the mouse had changed them there
        movzx   ebx,byte ptr [kt_btn]
        and     ebx,7
        xor     ecx,ecx
        xor     edx,edx
        mov     eax,0F00Bh
        int     21h
        call    ms_poll
        test    byte ptr [kt_btn],1
        jnz     mtt_out
        cmp     dword ptr [mt_wasdown],0
        jne     mtt_up
        mov     byte ptr [ms_click],0   ; up and staying up
        jmp     short mtt_out
mtt_up:
        movzx   eax,byte ptr [kt_row]
        cmp     eax,[mt_prow]
        jne     mtt_out
        movzx   eax,byte ptr [kt_col]
        cmp     eax,[mt_pcol]
        jne     mtt_out
        mov     byte ptr [ms_click],1   ; let go where it was pressed
mtt_out:
        popad
        ret

;---------------------------------------------------------------------
; ms_poll - what the driver has to say since the last look, queued as
;           events in the order they happened: a button down now was
;           pressed last, one up now was let go last.
;---------------------------------------------------------------------
ms_poll:
        pushad
        cmp     byte ptr [ms_on],0
        je      mp_out
        mov     eax,5
        xor     ebx,ebx
        int     33h
        movzx   ebx,bx
        mov     [mp_npress],ebx
        call    ms_cell
        mov     [mp_prow],edx
        mov     [mp_pcol],ecx
        mov     eax,6
        xor     ebx,ebx
        int     33h
        movzx   ebx,bx
        mov     [mp_nrel],ebx
        call    ms_cell
        mov     [mp_rrow],edx
        mov     [mp_rcol],ecx
        mov     eax,3
        int     33h
        and     ebx,1
        mov     [mp_btn],ebx
        call    ms_cell
        mov     [mp_row],edx
        mov     [mp_col],ecx
        cmp     dword ptr [mp_btn],0
        je      mp_isup
        ; down now
        cmp     dword ptr [mp_nrel],0
        je      mp_dpress
        cmp     dword ptr [ms_down],0
        je      mp_dpress
        mov     eax,EV_UP
        mov     edx,[mp_rrow]
        mov     ecx,[mp_rcol]
        call    mq_push
mp_dpress:
        cmp     dword ptr [mp_npress],0
        je      mp_after
        mov     eax,EV_DOWN
        mov     edx,[mp_prow]
        mov     ecx,[mp_pcol]
        call    mq_push
        jmp     short mp_after
mp_isup:
        cmp     dword ptr [mp_npress],0
        je      mp_urel
        mov     eax,EV_DOWN
        mov     edx,[mp_prow]
        mov     ecx,[mp_pcol]
        call    mq_push
mp_urel:
        cmp     dword ptr [mp_nrel],0
        je      mp_after
        cmp     dword ptr [ms_down],0
        jne     mp_urelq
        cmp     dword ptr [mp_npress],0
        je      mp_after
mp_urelq:
        mov     eax,EV_UP
        mov     edx,[mp_rrow]
        mov     ecx,[mp_rcol]
        call    mq_push
mp_after:
        mov     eax,[mp_btn]
        mov     [ms_down],eax
        or      eax,eax
        jz      mp_where
        cmp     dword ptr [mp_npress],0
        jne     mp_where
        mov     edx,[mp_row]
        mov     ecx,[mp_col]
        cmp     edx,[ms_row]
        jne     mp_drag
        cmp     ecx,[ms_col]
        je      mp_where
mp_drag:
        mov     eax,EV_DRAG
        call    mq_push
mp_where:
        mov     eax,[mp_row]
        mov     [ms_row],eax
        mov     eax,[mp_col]
        mov     [ms_col],eax
mp_out:
        popad
        ret

;--- ms_cell - ECX = x, EDX = y in the driver's units -> ECX = the
;--- column, EDX = the row
ms_cell:
        push    eax
        push    ebx
        push    esi
        mov     esi,edx
        mov     eax,ecx
        or      eax,eax
        jns     msc_x
        xor     eax,eax
msc_x:
        mul     dword ptr [ms_cols]
        mov     ebx,[ms_xmax]
        inc     ebx
        div     ebx
        cmp     eax,[ms_cols]
        jb      msc_xok
        mov     eax,[ms_cols]
        dec     eax
msc_xok:
        mov     ecx,eax
        mov     eax,esi
        or      eax,eax
        jns     msc_y
        xor     eax,eax
msc_y:
        mul     dword ptr [ms_rows]
        mov     ebx,[ms_ymax]
        inc     ebx
        div     ebx
        cmp     eax,[ms_rows]
        jb      msc_yok
        mov     eax,[ms_rows]
        dec     eax
msc_yok:
        mov     edx,eax
        pop     esi
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; mq_push - EAX = an event, EDX = its row, ECX = its column, onto the
;           queue.  A press on the cell of the last one, soon enough, is
;           a double one - "soon" being DBL_TIME on the clock, or in a
;           test a click with nothing typed since.  A full queue loses
;           the event; only a drag can be quick enough to fill it.
;---------------------------------------------------------------------
mq_push:
        pushad
        cmp     eax,EV_DOWN
        jne     mqp_store
        cmp     edx,[ms_lastrow]
        jne     mqp_first
        cmp     ecx,[ms_lastcol]
        jne     mqp_first
        cmp     byte ptr [kt_on],0
        je      mqp_clock
        cmp     byte ptr [ms_click],0
        je      mqp_first
        jmp     short mqp_double
mqp_clock:
        call    ev_now
        sub     eax,[ms_lasttime]
        cmp     eax,DBL_TIME
        mov     eax,EV_DOWN
        jae     mqp_first
mqp_double:
        mov     eax,EV_DOUBLE
        mov     dword ptr [ms_lastrow],-1   ; and a third press starts again
        jmp     short mqp_stamp
mqp_first:
        mov     eax,EV_DOWN
        mov     [ms_lastrow],edx
        mov     [ms_lastcol],ecx
mqp_stamp:
        push    eax
        call    ev_now
        mov     [ms_lasttime],eax
        pop     eax
mqp_store:
        mov     ebx,[mq_tail]
        lea     esi,[ebx+1]
        cmp     esi,MQ_MAX
        jb      mqp_wrap
        xor     esi,esi
mqp_wrap:
        cmp     esi,[mq_head]
        je      mqp_out                 ; full
        imul    edi,ebx,MQ_LEN
        add     edi,OFFSET mq_buf
        mov     [edi],eax
        mov     [edi+4],edx
        mov     [edi+8],ecx
        mov     [mq_tail],esi
mqp_out:
        popad
        ret

;--- mq_pop - EAX = the oldest event queued (ev_row and ev_col set),
;--- or 0 when there is none
mq_pop:
        push    ebx
        mov     ebx,[mq_head]
        cmp     ebx,[mq_tail]
        je      mqo_none
        imul    eax,ebx,MQ_LEN
        add     eax,OFFSET mq_buf
        push    dword ptr [eax+4]
        pop     dword ptr [ev_row]
        push    dword ptr [eax+8]
        pop     dword ptr [ev_col]
        mov     eax,[eax]
        inc     ebx
        cmp     ebx,MQ_MAX
        jb      mqo_set
        xor     ebx,ebx
mqo_set:
        mov     [mq_head],ebx
        pop     ebx
        ret
mqo_none:
        xor     eax,eax
        pop     ebx
        ret

;--- ev_now - hundredths of a second since midnight, in EAX ----------
ev_now:
        push    ebx
        push    ecx
        push    edx
        mov     eax,2C00h
        int     21h
        movzx   eax,ch
        imul    eax,60
        movzx   ebx,cl
        add     eax,ebx
        imul    eax,60
        movzx   ebx,dh
        add     eax,ebx
        imul    eax,100
        movzx   ebx,dl
        add     eax,ebx
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- tk_byte - one byte of standard input into AL; CF at the end -----
tk_byte:
        push    ebx
        push    ecx
        push    edx
        mov     ebx,0
        mov     ecx,1
        mov     edx,OFFSET kt_rec
        mov     eax,3F00h
        int     21h
        jc      tkb_eof
        or      eax,eax
        jz      tkb_eof
        mov     al,byte ptr [kt_rec]
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
tkb_eof:
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;--- tui_ktinit - FDTEST=path in the environment turns the file on, and
;--- FDHOLD=anything keeps the last screen up when it runs out: the
;--- keyboard takes over, which nobody is typing at, so a capture of
;--- the screen ends on whatever the file left showing
tui_ktinit:
        pushad
        mov     edi,OFFSET msg_tenv
        call    env_find
        jc      tki_out
        mov     edx,ebx                 ; the path: created (truncating),
        mov     ecx,0                   ; and the handle kept
        mov     eax,3C00h
        int     21h
        jc      tki_out
        mov     [kt_h],eax
        mov     byte ptr [kt_on],1
        mov     edi,OFFSET msg_thold
        call    env_find
        jc      tki_out
        mov     byte ptr [kt_hold],1
tki_out:
        popad
        ret

;--- env_find - EDI -> "NAME=" and a 0.  CF clear and EBX -> its value
;--- when the environment has it.
env_find:
        push    eax
        push    edx
        push    esi
        push    edi
        mov     esi,SLOT_PSP + PSP_ENV  ; a bracketed constant would be an
        mov     esi,[esi]               ; immediate: load the address first
        or      esi,esi
        jz      enf_no
enf_var:
        cmp     byte ptr [esi],0
        je      enf_no
        mov     edx,edi
        mov     ebx,esi
enf_cmp:
        mov     al,[edx]
        or      al,al
        jz      enf_yes
        mov     ah,[ebx]
        cmp     ah,'a'
        jb      enf_cmpc
        cmp     ah,'z'
        ja      enf_cmpc
        sub     ah,20h
enf_cmpc:
        cmp     al,ah
        jne     enf_next
        inc     edx
        inc     ebx
        jmp     short enf_cmp
enf_next:
        cmp     byte ptr [esi],0
        je      enf_skip
        inc     esi
        jmp     short enf_next
enf_skip:
        inc     esi
        jmp     short enf_var
enf_yes:
        pop     edi
        pop     esi
        pop     edx
        pop     eax
        clc
        ret
enf_no:
        pop     edi
        pop     esi
        pop     edx
        pop     eax
        stc
        ret

;--- tui_dump - the screen as text, appended to the log --------------
tui_dump:
        pushad
        cmp     byte ptr [kt_on],0
        je      td_out
        mov     esi,OFFSET scr_buf
        xor     ebp,ebp
td_row:
        mov     edi,OFFSET kt_line
        mov     ecx,TUI_COLS
td_ch:
        lodsw
        stosb
        dec     ecx
        jnz     td_ch
        mov     ecx,TUI_COLS            ; trailing blanks off, then CR LF
td_trim:
        cmp     byte ptr [edi-1],' '
        jne     td_trimmed
        dec     edi
        dec     ecx
        jnz     td_trim
td_trimmed:
        mov     byte ptr [edi],13
        mov     byte ptr [edi+1],10
        add     ecx,2
        mov     edx,OFFSET kt_line
        mov     ebx,[kt_h]
        mov     eax,4000h
        int     21h
        inc     ebp
        cmp     ebp,TUI_ROWS
        jb      td_row
        mov     edx,OFFSET msg_tdumpend
        mov     ecx,6                   ; four dashes and a new line
        mov     ebx,[kt_h]
        mov     eax,4000h
        int     21h
td_out:
        popad
        ret

;--- the cursor: off while the screen is up, back as it was after ----
tui_hidecur:
        pushad
        mov     ecx,2000h
        mov     eax,0F006h
        int     21h
        popad
        ret

tui_showcur:
        pushad
        mov     eax,0F003h              ; CL = the character height
        int     21h
        movzx   ecx,cl
        cmp     ecx,4
        jb      tsc_def
        cmp     ecx,32
        jbe     tsc_have
tsc_def:
        mov     ecx,16
tsc_have:
        mov     eax,ecx
        dec     eax                     ; the last line...
        sub     ecx,2                   ; ...and the one above it
        shl     ecx,8
        or      ecx,eax
        mov     eax,0F006h
        int     21h
        popad
        ret

;=====================================================================
; Data for the keyboard and the mouse.
;=====================================================================
kt_on       db  0                       ; keys from the file
kt_hold     db  0                       ; ...and then the keyboard
ms_on       db  0                       ; there is a mouse
ms_click    db  0                       ; test mode: the last thing was a click
ev_shift    db  0
kt_btn      db  0
kt_row      db  0
kt_col      db  0
            db  0
ev_kcode      dw  0
            dw  0
ev_row      dd  0
ev_col      dd  0
kt_h        dd  0
kt_rec      dd  0
ms_rows     dd  TUI_ROWS
ms_cols     dd  TUI_COLS
ms_xmax     dd  639
ms_ymax     dd  199
ms_down     dd  0
ms_row      dd  -1
ms_col      dd  -1
ms_lastrow  dd  -1
ms_lastcol  dd  -1
ms_lasttime dd  0
mt_wasdown  dd  0
mt_prow     dd  0
mt_pcol     dd  0
mp_npress   dd  0
mp_nrel     dd  0
mp_prow     dd  0
mp_pcol     dd  0
mp_rrow     dd  0
mp_rcol     dd  0
mp_btn      dd  0
mp_row      dd  0
mp_col      dd  0
mq_head     dd  0
mq_tail     dd  0
mq_buf      db  MQ_MAX * MQ_LEN dup(0)
