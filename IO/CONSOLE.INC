;=====================================================================
; CONSOLE.INC - VGA text mode console driver
;
; Writes straight to video memory rather than going through the BIOS,
; so it works with interrupts off and inside exception handlers.
;
; BUT IT DOES NOT OWN THE SCREEN.  A DOS box's INT 10h reflects to the
; real BIOS, which moves the hardware cursor and records where it left
; it in the BIOS data area - so a console that kept its own cursor in
; its own variables was right only until the first guest wrote a
; character.  After that the two disagreed for ever: the shell's next
; prompt landed wherever the kernel had left off, on top of whatever
; the program had printed since, and a guest asking INT 10h/03h where
; the cursor was got an answer that had nothing to do with the one on
; the screen.
;
; So the BIOS data area is the truth and this driver is one of its two
; users.  Every entry point PULLS the cursor and the screen geometry
; out of the BDA before it does anything, and PUBLISHES them again
; afterwards - to the BDA and to the CRT controller both.  cur_row and
; cur_col still exist, but as working copies for the span of one call,
; not as the record of where the cursor is.
;
; The page matters as well as the position: the BDA says which display
; page is live and at what offset, and following it means a guest that
; flips to page 1 gets kernel output on the page it is looking at.
;
; NOT handled, and known: a guest that leaves the screen in a GRAPHICS
; mode.  Text written by this driver then goes into the framebuffer as
; character cells and comes out as noise.  Doing better means routing
; con_out through the BIOS teletype, which cannot be done from the
; panic and exception paths this driver has to keep working in.  The
; geometry check below at least stops it scribbling outside the page.
;
; Included by IOPM.ASM.
;=====================================================================

;=====================================================================
; The cursor and the geometry are WORKING COPIES, refreshed from the
; BDA by con_sync at the top of every call.  Nothing outside this file
; should read them: what they hold between calls is last time's answer.
;
; AT THE TOP, and not at the foot of the file with everything else's
; data, because wasm 1.9 fixes the type of a symbol at its first
; mention: a forward reference from inside a routine gets guessed at,
; and the guess survives the declaration that follows it.  Declaring
; them first is the fix that keeps working when the code above them
; changes.  Nothing falls into here - VM86.INC ends in its own data,
; and con_init is only ever called.
;=====================================================================
cur_row     dd  0
cur_col     dd  0
con_cols    dd  SCR_COLS
con_rows    dd  SCR_ROWS
con_vbase   dd  VGA_TEXT                ; adapter base + live page offset
con_pgoff   dd  0                       ; ...and that offset on its own
con_crtcp   dd  CRTC_IDX
cur_attr    db  SCR_ATTR
            db  0,0,0

;=====================================================================
; con_init - clear the screen and home the cursor.
;=====================================================================
con_init:
        mov     byte ptr [cur_attr],SCR_ATTR
        ; fall through - con_cls homes the cursor and publishes it

;=====================================================================
; con_cls - clear the screen, home the cursor.
;=====================================================================
con_cls:
        pushad
        call    con_sync
        mov     edi,[con_vbase]
        mov     ecx,[con_cols]
        imul    ecx,[con_rows]
        movzx   eax,byte ptr [cur_attr]
        shl     eax,8
        mov     al,' '
        rep     stosw
        mov     dword ptr [cur_row],0
        mov     dword ptr [cur_col],0
        call    con_curupd
        popad
        ret

;=====================================================================
; con_out - write AL to the screen, honouring CR, LF, BS and TAB.
;           All registers preserved.
;=====================================================================
con_out:
        pushad
        movzx   ebx,al
        call    con_sync                ; where did the BIOS leave it?

        cmp     bl,13
        je      co_cr
        cmp     bl,10
        je      co_lf
        cmp     bl,8
        je      co_bs
        cmp     bl,9
        je      co_tab
        cmp     bl,7
        je      co_out                  ; bell - nothing to ring yet
        cmp     bl,32
        jb      co_out                  ; drop other control codes

        call    co_putc
        jmp     short co_upd

co_cr:
        mov     dword ptr [cur_col],0
        jmp     short co_upd

co_lf:
        call    co_newline
        jmp     short co_upd

co_bs:
        cmp     dword ptr [cur_col],0
        je      co_upd
        dec     dword ptr [cur_col]
        call    co_offset
        mov     ah,byte ptr [cur_attr]
        mov     al,' '
        mov     [edi],ax
        jmp     short co_upd

co_tab:
        mov     eax,[cur_col]
        add     eax,8
        and     eax,NOT 7
        cmp     eax,[con_cols]
        jb      co_tab1
        mov     dword ptr [cur_col],0
        call    co_newline
        jmp     short co_upd
co_tab1:
        mov     dword ptr [cur_col],eax

co_upd:
        call    con_curupd
co_out:
        popad
        ret

;=====================================================================
; con_sync - adopt the BIOS data area's idea of the screen.
;
; Geometry first, because the cursor has to be clamped against it, and
; every field is checked rather than believed: this is memory a guest
; can write, and a nonsense column count would send the arithmetic
; below straight off the end of the page.  Anything impossible falls
; back to the 80x25 the kernel booted with.
;=====================================================================
con_sync:
        push    eax
        push    ebx
        push    edx

;--- columns ---------------------------------------------------------
        movzx   eax,word ptr ds:[BDA_VCOLS]
        or      eax,eax
        jz      cy_defcols
        cmp     eax,SCR_MAXCOLS
        ja      cy_defcols
        jmp     short cy_cols
cy_defcols:
        mov     eax,SCR_COLS
cy_cols:
        mov     dword ptr [con_cols],eax

;--- rows, which the BDA stores one short ----------------------------
        movzx   eax,byte ptr ds:[BDA_VROWS]
        inc     eax
        cmp     eax,2                   ; a one-row screen is not a screen
        jb      cy_defrows
        cmp     eax,SCR_MAXROWS
        jbe     cy_rows
cy_defrows:
        mov     eax,SCR_ROWS
cy_rows:
        mov     dword ptr [con_rows],eax

;--- where the live page starts, and which adapter it is on ----------
        mov     ebx,VGA_TEXT
        mov     edx,CRTC_IDX
        cmp     byte ptr ds:[BDA_VMODE],7       ; mode 7 is the mono card,
        jne     cy_colour                       ; a different 32K entirely
        mov     ebx,VGA_MONO
        mov     edx,3B4h
cy_colour:
        mov     dword ptr [con_crtcp],edx

        movzx   eax,word ptr ds:[BDA_VPGOFF]
        mov     edx,[con_cols]
        imul    edx,[con_rows]
        shl     edx,1                           ; bytes in one page
        cmp     eax,8000h                       ; the offset has to leave a
        jae     cy_page0                        ; whole page inside the 32K
        add     eax,edx
        cmp     eax,8000h
        ja      cy_page0
        sub     eax,edx
        jmp     short cy_pgok
cy_page0:
        xor     eax,eax
cy_pgok:
        mov     dword ptr [con_pgoff],eax
        add     ebx,eax
        mov     dword ptr [con_vbase],ebx

;--- and the cursor itself, for the page that is being displayed -----
        movzx   ebx,byte ptr ds:[BDA_VPAGE]
        and     ebx,7                           ; eight pages, no more
        shl     ebx,1
        add     ebx,BDA_CURPOS
        movzx   eax,byte ptr ds:[ebx+1]         ; row
        cmp     eax,[con_rows]
        jb      cy_row
        mov     eax,[con_rows]
        dec     eax
cy_row:
        mov     dword ptr [cur_row],eax
        movzx   eax,byte ptr ds:[ebx]           ; column
        cmp     eax,[con_cols]
        jb      cy_col
        mov     eax,[con_cols]
        dec     eax
cy_col:
        mov     dword ptr [cur_col],eax

        pop     edx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; co_offset - EDI = the linear address of the current cursor cell.
;---------------------------------------------------------------------
co_offset:
        push    eax
        mov     eax,[cur_row]
        imul    eax,[con_cols]
        add     eax,[cur_col]
        shl     eax,1
        add     eax,[con_vbase]
        mov     edi,eax
        pop     eax
        ret

;---------------------------------------------------------------------
; co_putc - store BL at the cursor and advance, wrapping to the next
;           line at the right margin.
;---------------------------------------------------------------------
co_putc:
        call    co_offset
        mov     al,bl
        mov     ah,byte ptr [cur_attr]
        mov     [edi],ax
        push    eax
        mov     eax,[cur_col]
        inc     eax
        mov     dword ptr [cur_col],eax
        cmp     eax,[con_cols]
        pop     eax
        jb      cp_done
        mov     dword ptr [cur_col],0
        call    co_newline
cp_done:
        ret

;---------------------------------------------------------------------
; co_newline - down one row, scrolling if we fall off the bottom.
;---------------------------------------------------------------------
co_newline:
        push    eax
        mov     eax,[cur_row]
        inc     eax
        mov     dword ptr [cur_row],eax
        cmp     eax,[con_rows]
        jb      cn_done
        call    con_scroll
        mov     eax,[con_rows]
        dec     eax
        mov     dword ptr [cur_row],eax
cn_done:
        pop     eax
        ret

;---------------------------------------------------------------------
; con_scroll - pull the screen up one line and blank the last.
;---------------------------------------------------------------------
con_scroll:
        push    esi
        push    edi
        push    ecx
        push    edx
        push    eax

        mov     ecx,[con_cols]
        shl     ecx,1                   ; bytes in one row
        mov     edx,ecx

        mov     esi,[con_vbase]
        add     esi,edx
        mov     edi,[con_vbase]
        mov     ecx,[con_rows]
        dec     ecx
        imul    ecx,edx
        add     ecx,3                   ; ...as dwords, rounded UP: the last
        shr     ecx,2                   ; row gets blanked below anyway, and
        rep     movsd                   ; truncating would leave a stripe

        mov     edi,[con_vbase]         ; the last row, blanked
        mov     eax,[con_rows]
        dec     eax
        imul    eax,edx
        add     edi,eax
        mov     ecx,[con_cols]
        movzx   eax,byte ptr [cur_attr]
        shl     eax,8
        mov     al,' '
        rep     stosw

        pop     eax
        pop     edx
        pop     ecx
        pop     edi
        pop     esi
        ret

;=====================================================================
; con_str - write the ASCIIZ string at ESI.  All registers preserved.
;=====================================================================
con_str:
        pushad
cs_lp:
        mov     al,[esi]
        or      al,al
        jz      cs_done
        call    con_out
        inc     esi
        jmp     short cs_lp
cs_done:
        popad
        ret

;=====================================================================
; con_setcur - DH = row, DL = column.
;=====================================================================
con_setcur:
        pushad
        call    con_sync                ; for the geometry to clamp against
        movzx   eax,dh
        cmp     eax,[con_rows]
        jb      sc_row
        mov     eax,[con_rows]
        dec     eax
sc_row:
        mov     dword ptr [cur_row],eax
        movzx   eax,dl
        cmp     eax,[con_cols]
        jb      sc_col
        mov     eax,[con_cols]
        dec     eax
sc_col:
        mov     dword ptr [cur_col],eax
        call    con_curupd
        popad
        ret

;=====================================================================
; con_getcur - DH = row, DL = column.
;
; Straight out of the BDA, which is the whole point: what this returns
; has to be where the cursor IS, including when the last thing to move
; it was a DOS box calling INT 10h.
;=====================================================================
con_getcur:
        call    con_sync
        mov     dh,byte ptr [cur_row]
        mov     dl,byte ptr [cur_col]
        ret

;=====================================================================
; con_getsize - DH = rows, DL = columns, as the screen is NOW.
;
; For anything that pages its output: DIR /P counting 25 lines onto a
; 50-line screen stops twice as often as it should, and onto a 43-line
; one it stops in the wrong places.
;=====================================================================
con_getsize:
        call    con_sync
        mov     dh,byte ptr [con_rows]
        mov     dl,byte ptr [con_cols]
        ret

;=====================================================================
; con_curupd - publish the cursor: to the BIOS data area, so that the
;              BIOS and any guest asking it agree with us, and to the
;              CRT controller, which is what actually moves it.
;=====================================================================
con_curupd:
        push    eax
        push    ebx
        push    edx

;--- the BDA slot for the page being displayed -----------------------
        movzx   ebx,byte ptr ds:[BDA_VPAGE]
        and     ebx,7
        shl     ebx,1
        add     ebx,BDA_CURPOS
        mov     al,byte ptr [cur_col]
        mov     byte ptr ds:[ebx],al
        mov     al,byte ptr [cur_row]
        mov     byte ptr ds:[ebx+1],al

;--- and the CRTC, which counts cells from the start of the adapter's
;--- memory and not from the start of the page
        mov     eax,[cur_row]
        imul    eax,[con_cols]
        add     eax,[cur_col]
        mov     ebx,[con_pgoff]
        shr     ebx,1                   ; the page offset, in cells
        add     ebx,eax                 ; EBX = absolute cell number

        mov     edx,[con_crtcp]
        mov     al,0Fh
        out     dx,al
        inc     edx
        mov     al,bl
        out     dx,al

        mov     edx,[con_crtcp]
        mov     al,0Eh
        out     dx,al
        inc     edx
        mov     al,bh
        out     dx,al

        pop     edx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; con_hex32 / con_hex16 / con_hex8 - print the low 8, 4 or 2 nibbles
; of EAX in hex.  All registers preserved.
;=====================================================================
con_hex32:
        push    ecx
        mov     ecx,8
        jmp     short ch_go
con_hex16:
        push    ecx
        mov     ecx,4
        jmp     short ch_go
con_hex8:
        push    ecx
        mov     ecx,2
ch_go:
        push    eax
        push    ebx
        push    edx

        mov     ebx,eax
        mov     edx,ecx                 ; EDX = digits wanted
        mov     ecx,8
        sub     ecx,edx                 ; nibbles to discard
        shl     ecx,2
        shl     ebx,cl                  ; left-justify the ones we want
        mov     ecx,edx
ch_lp:
        rol     ebx,4                   ; next nibble into BL's low half
        mov     al,bl
        and     al,0Fh
        cmp     al,10
        jb      ch_dig
        add     al,7                    ; 'A'-'0'-10
ch_dig:
        add     al,'0'
        call    con_out
        loop    ch_lp

        pop     edx
        pop     ebx
        pop     eax
        pop     ecx
        ret

;=====================================================================
; con_dec - print EAX as an unsigned decimal number.
;=====================================================================
con_dec:
        pushad
        mov     ecx,0
        mov     ebx,10
cd_div:
        xor     edx,edx
        div     ebx
        push    edx
        inc     ecx
        or      eax,eax
        jnz     cd_div
cd_out:
        pop     eax
        add     al,'0'
        call    con_out
        loop    cd_out
        popad
        ret

