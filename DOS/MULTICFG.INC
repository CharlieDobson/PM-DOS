;=====================================================================
; MULTICFG.INC - more than one configuration in one CONFIG.SYS
;
; MS-DOS 6's multi-config, as that DOS shipped it (S:\DOS6SRC,
; bios\sysinit2.asm, menu_check and its helpers):
;
;   [name]          starts a block, which runs to the next [name]
;   [COMMON]        a block every configuration uses; so are the lines
;                   above the first block
;   [MENU]          the startup menu: MENUITEM=, SUBMENU=, MENUDEFAULT=,
;                   MENUCOLOR= and NUMLOCK=, and nothing else
;   INCLUDE=name    another block's lines, here
;   CONFIG=name     in the environment the shell is given, so that
;                   AUTOEXEC.BAT can say GOTO %CONFIG%
;
; and the interactive start that came with it: F8 at startup or on the
; menu asks about every line, a '?' after a statement's name asks about
; that one (DEVICE?=), and with F8 AUTOEXEC.BAT is asked about too and
; stepped through by the shell (CMD /Y).
;
; DOS DID IT BY REWRITING THE FILE, and this does it by reading it.
; menu_check made a second image of CONFIG.SYS holding only the chosen
; lines, with each INCLUDE spliced in, and sysinit then read that.
; Here the walks over cfg_buf (CONFIG.INC) are told which block a line
; is in and whether that block counts (mcf_header), and an INCLUDE=
; line walks the block it names there and then (mcf_include).  The
; result is the same lines in the same order, with no second 16K
; buffer, and the four walks CONFIG.INC already makes stay four walks.
;
; WHAT IS NOT DOS'S, AND WHY:
;
;   - An INCLUDE that would go round in a circle is refused with a
;     message.  DOS turned each INCLUDE line into a REM as it used it,
;     which stopped the loop but ran some lines twice on the way.
;   - A chain of SUBMENU defaults with no timeout and no display stops
;     after MCF_MAXHOPS menus instead of looping for ever.
;   - After the menu, the cells still in its colour go back to the
;     console's.  DOS left the screen coloured and its BIOS teletype
;     kept writing in whatever colour a cell already had; PM-DOS's
;     console writes every character in its own, which over a MENUCOLOR
;     screen would leave a patchwork.
;
; Included by DOSMAIN.ASM, after CONFIG.INC.
;=====================================================================

MCF_ITEMS       EQU     9               ; choices on one menu, as DOS
MCF_INCMAX      EQU     8               ; INCLUDEs inside INCLUDEs
MCF_MAXHOPS     EQU     16              ; SUBMENUs taken without a menu shown
MCF_NAMEMAX     EQU     64              ; of a block name, into CONFIG=
MCFK_BLOCK      EQU     1               ; what a choice leads to: a block,
MCFK_MENU       EQU     2               ; or another menu
MCF_BYPASS      EQU     0FFFFFFFFh      ; mcf_pick: F5
MCF_TOP         EQU     0FFFFFFFEh      ; mcf_pick: Backspace, the top menu

; Where things go on the menu screen.  DOS's positions: the items in
; column 5 from row 4, the prompt two rows under the last one.
MCF_TROW        EQU     1               ; the title, and its line under it
MCF_LCOL        EQU     2               ; the title's and the prompt's column
MCF_IROW        EQU     3               ; item N is on row MCF_IROW + N
MCF_ICOL        EQU     5
MCF_IEND        EQU     78              ; a description stops short of here
MCF_TIMECOL     EQU     51              ; "Time remaining: NN"

;=====================================================================
; mcf_choose - the [MENU] block, if there is one: show it, take a
;              choice, and say which block the walks are to use.
;              CF set if F5 on the menu bypassed CONFIG.SYS and
;              AUTOEXEC.BAT - which is done, and said, here.
;
; Called once, after the startup keys and before any statement is
; acted on.  With no [MENU] block only the common lines count and no
; CONFIG variable is set, both as DOS.  A menu with nothing valid on it
; is the same but for CONFIG=COMMON, which is DOS's answer there too.
;=====================================================================
mcf_choose:
        xor     eax,eax
        mov     [mcf_sel],eax
        mov     [mcf_selblk],eax
        mov     [mcf_hops],eax
        mov     esi,OFFSET mcfn_menu
        call    mcf_find
        jc      mch_none
mch_top:
        mov     [mcf_blk],edi
        call    mcf_parse
        cmp     dword ptr [mcf_count],0
        je      mch_nothing

        ; MENUDEFAULT's timeout of 0 means "the default, unseen" - unless
        ; F8 asked for the start to be confirmed, when it is shown anyway
        cmp     dword ptr [mcf_tmo],0
        jne     mch_show
        test    byte ptr [mcf_asks],CFGA_EVERY
        jz      mch_dflt
mch_show:
        mov     dword ptr [mcf_hops],0
        call    mcf_pause
        mov     eax,[mcf_dflt]
        mov     [mcf_cur],eax
        call    mcf_draw
        call    mcf_pick
        cmp     eax,MCF_BYPASS
        je      mch_bypass
        cmp     eax,MCF_TOP
        je      mch_again
        call    mcf_leave
        mov     eax,[mcf_cur]
        jmp     short mch_have
mch_dflt:
        mov     eax,[mcf_dflt]
mch_have:
        dec     eax
        mov     esi,[mcf_name+eax*4]
        cmp     byte ptr [mcf_kind+eax],MCFK_MENU
        jne     mch_item

        ; ANOTHER MENU, which is a block of its own read the same way
        inc     dword ptr [mcf_hops]
        cmp     dword ptr [mcf_hops],MCF_MAXHOPS
        ja      mch_nothing
        call    mcf_find                ; mcf_parse saw it there
        jc      mch_nothing
        jmp     mch_top

mch_item:
        mov     [mcf_sel],esi
        call    mcf_find
        mov     [mcf_selblk],edi        ; for INCLUDE, which may not name it
        jmp     short mch_env
mch_nothing:
        mov     dword ptr [mcf_sel],0
        mov     esi,OFFSET mcfn_common
mch_env:
        call    mcf_setenv
mch_none:
        clc
        ret

        ; Backspace: the top menu again, from wherever this one is
mch_again:
        mov     esi,OFFSET mcfn_menu
        call    mcf_find
        jmp     mch_top

mch_bypass:
        call    mcf_leave
        call    cfg_bypass
        stc
        ret

;=====================================================================
; mcf_parse - read the menu block whose first line is at [mcf_blk]:
;             its choices, which one is the default, its timeout and
;             its colours.  A statement that has no business in a menu
;             is complained about and passed over, as DOS did.
;=====================================================================
mcf_parse:
        xor     eax,eax
        mov     [mcf_count],eax
        mov     [mcf_dfname],eax
        mov     dword ptr [mcf_tmo],-1  ; no timeout unless MENUDEFAULT
        mov     esi,[mcf_blk]           ; gives one
mps_line:
        call    cfg_getline
        jc      mps_done
        push    esi
        call    mcf_menuline
        pop     esi
        jnc     mps_line                ; CF: a [block] line ended the menu

mps_done:
        ; The first choice is the default, unless MENUDEFAULT names one
        ; that is on the menu.
        mov     dword ptr [mcf_dflt],1
        mov     esi,[mcf_dfname]
        or      esi,esi
        jz      mps_out
        xor     ebx,ebx
mps_dfl:
        cmp     ebx,[mcf_count]
        jae     mps_out
        mov     edi,[mcf_name+ebx*4]
        call    mcf_same
        je      mps_dfhit
        inc     ebx
        jmp     short mps_dfl
mps_dfhit:
        inc     ebx
        mov     [mcf_dflt],ebx
mps_out:
        ret

;---------------------------------------------------------------------
; mcf_menuline - one line of a menu block, in cfg_line_buf.  CF set if
;                it is a [block] line, which ends the menu.
;---------------------------------------------------------------------
mcf_menuline:
        mov     esi,OFFSET cfg_line_buf
        call    cfg_blanks
        mov     al,[esi]
        or      al,al
        jz      mml_ok
        cmp     al,';'
        je      mml_ok
        cmp     al,'['
        je      mml_end
        call    cfg_keyword
        jz      mml_ok

        mov     ebx,OFFSET mcf_mtable
mml_lp:
        mov     edi,[ebx]
        or      edi,edi
        jz      mml_bad
        mov     esi,OFFSET cfg_key
mml_cmp:
        mov     al,[esi]
        cmp     al,[edi]
        jne     mml_miss
        or      al,al
        jz      mml_hit
        inc     esi
        inc     edi
        jmp     short mml_cmp
mml_miss:
        add     ebx,8
        jmp     short mml_lp
mml_hit:
        mov     esi,[cfg_val]
        call    dword ptr [ebx+4]
mml_ok:
        clc
        ret
mml_end:
        stc
        ret
mml_bad:
        call    cfg_show_line
        mov     esi,OFFSET msgCfgBadCmd
        call    dword ptr ds:[IOS_CONSTR]
        clc
        ret

; What a menu block may hold.  REM is a remark here as anywhere.
mcf_mtable:
        dd      OFFSET cfgn_menuitem,OFFSET mml_item
        dd      OFFSET cfgn_submenu, OFFSET mml_sub
        dd      OFFSET cfgn_menudflt,OFFSET mml_default
        dd      OFFSET cfgn_menucolr,OFFSET mml_colour
        dd      OFFSET cfgn_numlock, OFFSET cfg_numlock
        dd      OFFSET cfgn_rem,     OFFSET cfg_rem
        dd      0, 0

;---------------------------------------------------------------------
; MENUITEM=block[,text] and SUBMENU=block[,text] - a choice.  ESI ->
; the value.  The block has to exist; the text is what the menu shows,
; and the block's name is shown if there is none.
;
; The name and the text are kept as pointers into cfg_buf, which
; outlives the line: cfg_getline copies a line verbatim, so a place in
; cfg_line_buf is the same distance from the line's start in cfg_buf.
;---------------------------------------------------------------------
mml_sub:
        mov     dl,MCFK_MENU
        jmp     short mml_add
mml_item:
        mov     dl,MCFK_BLOCK
mml_add:
        mov     ecx,[mcf_count]
        cmp     ecx,MCF_ITEMS
        jae     mma_out                 ; past nine, not shown - as DOS
        call    mcf_find
        jc      mma_noblk
        mov     eax,esi
        sub     eax,OFFSET cfg_line_buf
        add     eax,[cfg_lstart]
        mov     [mcf_name+ecx*4],eax
        mov     [mcf_desc+ecx*4],eax
        mov     [mcf_kind+ecx],dl
        call    mcf_skipname
        cmp     byte ptr [esi],0
        je      mma_count
        mov     eax,esi
        sub     eax,OFFSET cfg_line_buf
        add     eax,[cfg_lstart]
        mov     [mcf_desc+ecx*4],eax
mma_count:
        inc     dword ptr [mcf_count]
mma_out:
        ret
mma_noblk:
        call    cfg_show_line
        mov     esi,OFFSET msgMcfNoBlk
        call    dword ptr ds:[IOS_CONSTR]
        ret

;---------------------------------------------------------------------
; MENUDEFAULT=block[,seconds] - the choice that starts highlighted, and
; how long the menu waits before taking it.
;
; No number: it waits for ever.  A number over 90 is 90, which is
; DOS's ceiling.  Something there that is not a number counts as 0,
; as it did in DOS: the menu is not shown and the default is used.
; Once a key has been pressed at startup no menu has a timeout - the
; person is there.
;---------------------------------------------------------------------
mml_default:
        cmp     byte ptr [esi],0
        je      mdf_out
        mov     eax,esi
        sub     eax,OFFSET cfg_line_buf
        add     eax,[cfg_lstart]
        mov     [mcf_dfname],eax
        cmp     byte ptr [mcf_typed],0
        jne     mdf_out
        call    mcf_skipname
        cmp     byte ptr [esi],0
        je      mdf_out
        call    cfg_num
        jnc     mdf_num
        xor     eax,eax
mdf_num:
        cmp     eax,90
        jbe     mdf_set
        mov     eax,90
mdf_set:
        mov     [mcf_tmo],eax
mdf_out:
        ret

;---------------------------------------------------------------------
; MENUCOLOR=text[,screen] - the menu's colours, 0 to 15 each.  The
; screen is black if only the text is given.  The same colour for both
; would be invisible, so the text is made bright then, as DOS did.
;---------------------------------------------------------------------
mml_colour:
        call    cfg_num
        jnc     mco_text
        xor     eax,eax
mco_text:
        and     eax,0Fh
        mov     edx,eax
        xor     ebx,ebx
        mov     al,[esi]
        cmp     al,','
        je      mco_more
        cmp     al,' '
        je      mco_more
        cmp     al,9
        jne     mco_set
mco_more:
        inc     esi
        call    cfg_blanks
        cmp     byte ptr [esi],','
        je      mco_more
        call    cfg_num
        jc      mco_set
        and     eax,0Fh
        mov     ebx,eax
mco_set:
        mov     eax,ebx
        shl     eax,4
        or      eax,edx
        cmp     ebx,edx
        jne     mco_keep
        xor     al,08h
mco_keep:
        mov     [mcf_attr],al
        ret

;=====================================================================
; The menu screen
;=====================================================================

;---------------------------------------------------------------------
; mcf_draw - the whole menu, choice [mcf_cur] highlighted, the cursor
;            on the prompt.
;
; The layout is DOS's: a title with a double line under it, the
; choices numbered from row 4, "Enter a choice:" two rows under the
; last, and the keys F5 and F8 do on the bottom row with the state of
; F8 at its end.  The words are PM-DOS's.
;---------------------------------------------------------------------
mcf_draw:
        pushad
        mov     byte ptr [mcf_said],0
        call    dword ptr ds:[IOS_SCRSIZE]
        movzx   eax,dl
        cmp     eax,SCR_MAXCOLS         ; mcf_cells holds a row of this
        jbe     mdr_cols
        mov     eax,SCR_MAXCOLS
mdr_cols:
        mov     [mcf_cols],eax
        movzx   eax,dh
        mov     [mcf_rows],eax

        xor     edx,edx                 ; the whole screen in the menu's
mdr_clear:                              ; colour first
        mov     ah,[mcf_attr]
        mov     al,' '
        mov     ecx,[mcf_cols]
        call    mcf_run
        inc     dh
        movzx   eax,dh
        cmp     eax,[mcf_rows]
        jb      mdr_clear

        mov     dh,MCF_TROW
        mov     dl,MCF_LCOL
        mov     ah,[mcf_attr]
        mov     esi,OFFSET msgMcfTitle
        mov     ecx,SCR_MAXCOLS
        call    mcf_put                 ; -> ECX = its length
        inc     dh
        mov     al,0CDh                 ; a double line as long as it is
        call    mcf_run

        mov     eax,[mcf_count]         ; the prompt, two rows under the
        add     eax,MCF_IROW + 2        ; last choice
        mov     [mcf_prow],eax
        mov     dh,al
        mov     dl,MCF_LCOL
        mov     ah,[mcf_attr]
        mov     esi,OFFSET msgMcfPrompt
        mov     ecx,SCR_MAXCOLS
        call    mcf_put
        add     ecx,MCF_LCOL
        mov     [mcf_pcol],ecx

        cmp     byte ptr [cfg_nokeys],0 ; SWITCHES=/N: F5 and F8 do nothing,
        jne     mdr_items               ; so they are not offered
        mov     eax,[mcf_rows]
        dec     eax
        mov     dh,al
        xor     dl,dl
        mov     ah,[mcf_attr]
        mov     esi,OFFSET msgMcfKeys
        mov     ecx,[mcf_cols]
        call    mcf_put
        sub     ecx,2                   ; the N of "[N]"
        mov     [mcf_yncol],ecx
        call    mcf_status

mdr_items:
        mov     eax,1
mdr_item:
        cmp     eax,[mcf_count]
        ja      mdr_done
        call    mcf_item
        inc     eax
        jmp     short mdr_item
mdr_done:
        call    mcf_digit
        popad
        ret

;---------------------------------------------------------------------
; mcf_item - EAX = a choice, 1 to [mcf_count]: its line, highlighted if
;            it is [mcf_cur].  The highlight is a light grey screen
;            under the menu's text colour, made bright where the two
;            would be the same - DOS's rule.
;---------------------------------------------------------------------
mcf_item:
        pushad
        mov     ebx,eax
        mov     ah,[mcf_attr]
        cmp     ebx,[mcf_cur]
        jne     mit_put
        or      ah,70h
        mov     al,ah
        ror     al,4
        cmp     al,ah
        jne     mit_put
        xor     ah,08h
mit_put:
        mov     al,bl
        add     al,'0'
        mov     [mcf_num],al            ; "N. "
        mov     dh,bl
        add     dh,MCF_IROW
        mov     dl,MCF_ICOL
        mov     esi,OFFSET mcf_num
        mov     ecx,3
        call    mcf_put
        add     dl,3
        mov     esi,[mcf_desc+ebx*4-4]
        mov     ecx,MCF_IEND - MCF_ICOL - 3
        call    mcf_put
        popad
        ret

;---------------------------------------------------------------------
; mcf_digit - the current choice's number at the prompt, and the cursor
;             on it.
;---------------------------------------------------------------------
mcf_digit:
        pushad
        mov     eax,[mcf_prow]
        mov     dh,al
        mov     eax,[mcf_pcol]
        mov     dl,al
        mov     ah,[mcf_attr]
        mov     al,byte ptr [mcf_cur]
        add     al,'0'
        mov     ecx,1
        call    mcf_run
        call    dword ptr ds:[IOS_SETCUR]
        popad
        ret

;---------------------------------------------------------------------
; mcf_status - Y or N at the end of the bottom row: whether F8 has
;              asked for every line to be confirmed.
;---------------------------------------------------------------------
mcf_status:
        pushad
        cmp     byte ptr [cfg_nokeys],0
        jne     mst_out
        mov     eax,[mcf_rows]
        dec     eax
        mov     dh,al
        mov     eax,[mcf_yncol]
        mov     dl,al
        mov     ah,[mcf_attr]
        mov     al,'N'
        test    byte ptr [mcf_asks],CFGA_EVERY
        jz      mst_put
        mov     al,'Y'
mst_put:
        mov     ecx,1
        call    mcf_run
mst_out:
        popad
        ret

;---------------------------------------------------------------------
; mcf_showtime - EDX = seconds left: "Time remaining: NN" on the
;                prompt's row.  mcf_notime takes it off again.
;---------------------------------------------------------------------
mcf_showtime:
        pushad
        mov     eax,edx
        mov     cl,10
        div     cl                      ; AL = tens, AH = units: never
        add     ax,'00'                 ; more than 90, so this fits
        mov     word ptr [mcf_nn],ax
        mov     eax,[mcf_prow]
        mov     dh,al
        mov     dl,MCF_TIMECOL
        mov     ah,[mcf_attr]
        mov     esi,OFFSET msgMcfTime
        mov     ecx,SCR_MAXCOLS
        call    mcf_put
        add     dl,cl
        mov     esi,OFFSET mcf_nn
        mov     ecx,2
        call    mcf_put
        popad
        ret

mcf_notime:
        pushad
        mov     eax,[mcf_prow]
        mov     dh,al
        mov     dl,MCF_TIMECOL
        mov     ah,[mcf_attr]
        mov     al,' '
        mov     ecx,MCF_IEND - MCF_TIMECOL
        call    mcf_run
        popad
        ret

;---------------------------------------------------------------------
; mcf_pick - wait for a choice.  EAX = the choice, 1 to [mcf_count];
;            MCF_BYPASS for F5; MCF_TOP for Backspace.
;
; DOS's keys: the arrows move the highlight and a digit moves it
; straight to that choice, Enter takes it, F8 turns confirming every
; line on and off, F5 starts with neither file.  Any key stops the
; countdown, and then no later menu has one either.  When the time
; runs out with nobody there, the '?' questions are not asked: nobody
; is there to answer them.
;---------------------------------------------------------------------
mcf_pick:
        mov     byte ptr [mcf_ticking],0
        cmp     byte ptr [mcf_typed],0
        jne     mpk_wait
        cmp     dword ptr [mcf_tmo],-1
        je      mpk_wait
        mov     byte ptr [mcf_ticking],1
        call    dword ptr ds:[IOS_TICKS]
        mov     [mcf_t0],eax
        mov     dword ptr [mcf_shown],-1

mpk_wait:
        call    dword ptr ds:[IOS_CONSTAT]
        jnz     mpk_key
        cmp     byte ptr [mcf_ticking],0
        je      mpk_idle
        call    dword ptr ds:[IOS_TICKS]
        sub     eax,[mcf_t0]
        jae     mpk_since
        add     eax,TICKS_PER_DAY       ; past midnight since it started
mpk_since:
        imul    eax,10
        xor     edx,edx
        mov     ecx,182
        div     ecx                     ; whole seconds gone
        mov     edx,[mcf_tmo]
        sub     edx,eax                 ; and left - DOS shows 00 for a
        jb      mpk_timeout             ; second before it goes
        cmp     edx,[mcf_shown]
        je      mpk_idle
        mov     [mcf_shown],edx
        call    mcf_showtime
mpk_idle:
        sti
        hlt                             ; until the next tick or key
        jmp     short mpk_wait

mpk_timeout:
        or      byte ptr [mcf_asks],CFGA_YES
        mov     eax,[mcf_cur]
        ret

mpk_key:
        call    dword ptr ds:[IOS_CONIN]
        mov     byte ptr [mcf_typed],1
        cmp     byte ptr [mcf_ticking],0
        je      mpk_have
        mov     byte ptr [mcf_ticking],0
        call    mcf_notime
mpk_have:
        or      al,al
        jnz     mpk_char
        cmp     ah,48h                  ; up
        je      mpk_up
        cmp     ah,50h                  ; down
        je      mpk_down
        cmp     byte ptr [cfg_nokeys],0
        jne     mpk_wait
        cmp     ah,3Fh                  ; F5
        je      mpk_f5
        cmp     ah,42h                  ; F8
        jne     mpk_wait
        xor     byte ptr [mcf_asks],CFGA_EVERY
        call    mcf_status
        jmp     mpk_wait
mpk_f5:
        mov     eax,MCF_BYPASS
        ret

mpk_up:
        mov     eax,[mcf_cur]
        cmp     eax,1
        jbe     mpk_wait
        dec     eax
        jmp     short mpk_move
mpk_down:
        mov     eax,[mcf_cur]
        cmp     eax,[mcf_count]
        jae     mpk_wait
        inc     eax
        jmp     short mpk_move

mpk_char:
        cmp     al,13
        je      mpk_enter
        cmp     al,8
        je      mpk_back
        sub     al,'0'
        jbe     mpk_wait
        movzx   eax,al
        cmp     eax,[mcf_count]
        ja      mpk_wait
mpk_move:
        mov     ebx,[mcf_cur]
        mov     [mcf_cur],eax
        call    mcf_item                ; the new one lit...
        mov     eax,ebx
        call    mcf_item                ; ...and the old one not
        call    mcf_digit
        jmp     mpk_wait

mpk_enter:
        mov     eax,[mcf_cur]
        ret
mpk_back:
        mov     eax,MCF_TOP
        ret

;---------------------------------------------------------------------
; mcf_leave - a choice is made: the cursor goes to the line under the
;             prompt, where whatever CONFIG.SYS says next is written,
;             the keys line goes, and the menu's colour goes back to
;             the console's (see the note at the top).
;---------------------------------------------------------------------
mcf_leave:
        pushad
        mov     eax,[mcf_prow]
        inc     eax
        mov     dh,al
        xor     dl,dl
        call    dword ptr ds:[IOS_SETCUR]

        cmp     byte ptr [cfg_nokeys],0
        jne     mlv_colour
        mov     eax,[mcf_rows]
        dec     eax
        mov     dh,al
        xor     dl,dl
        mov     ah,[mcf_attr]
        mov     al,' '
        mov     ecx,[mcf_cols]
        call    mcf_run

mlv_colour:
        cmp     byte ptr [mcf_attr],SCR_ATTR
        je      mlv_out
        xor     edx,edx
mlv_row:
        mov     ebx,edx
        imul    ebx,[mcf_cols]
        mov     ecx,[mcf_cols]
        mov     edi,OFFSET mcf_cells
        call    dword ptr ds:[IOS_GETCELLS]
        mov     esi,OFFSET mcf_cells
        mov     al,[mcf_attr]
mlv_cell:
        cmp     [esi+1],al
        jne     mlv_next
        mov     byte ptr [esi+1],SCR_ATTR
mlv_next:
        add     esi,2
        loop    mlv_cell
        mov     ecx,[mcf_cols]
        mov     esi,OFFSET mcf_cells
        call    dword ptr ds:[IOS_PUTCELLS]
        inc     edx
        cmp     edx,[mcf_rows]
        jb      mlv_row
mlv_out:
        popad
        ret

;---------------------------------------------------------------------
; mcf_pause - if CONFIG.SYS has complained about anything since the
;             screen was last cleared, a key before the menu clears it,
;             so that the complaint can be read.  DOS paused here too.
;---------------------------------------------------------------------
mcf_pause:
        cmp     byte ptr [mcf_said],0
        je      mpa_out
        mov     byte ptr [mcf_said],0
        mov     esi,OFFSET msgMcfAnyKey
        call    dword ptr ds:[IOS_CONSTR]
        call    dword ptr ds:[IOS_CONIN]
        mov     esi,OFFSET msgCrLf
        call    dword ptr ds:[IOS_CONSTR]
mpa_out:
        ret

;---------------------------------------------------------------------
; mcf_put - ESI -> text, DH = row, DL = column, AH = colour, ECX = the
;           most cells to write.  The text ends at any control
;           character or a '$' (DOS's end for a menu description); a
;           tab is a space.  -> ECX = the cells written.
;
; mcf_run - DH = row, DL = column, AH = colour, AL = a character, ECX =
;           how many: a run of the one character.
;
; Both are clipped to a row's worth of cells, which is what mcf_cells
; holds; IOS_PUTCELLS clips again at the end of the screen.
;---------------------------------------------------------------------
mcf_put:
        pushad
        cmp     ecx,SCR_MAXCOLS
        jbe     mpu_room
        mov     ecx,SCR_MAXCOLS
mpu_room:
        mov     edi,OFFSET mcf_cells
        xor     ebx,ebx
mpu_lp:
        cmp     ebx,ecx
        jae     mpu_go
        mov     al,[esi]
        cmp     al,9
        jne     mpu_ntab
        mov     al,' '
mpu_ntab:
        cmp     al,' '
        jb      mpu_go
        cmp     al,'$'
        je      mpu_go
        mov     [edi+ebx*2],ax
        inc     ebx
        inc     esi
        jmp     short mpu_lp
mpu_go:
        mov     [esp+24],ebx            ; the ECX popad gives back
        mov     ecx,ebx
        jmp     short mcf_cellsout

mcf_run:
        pushad
        cmp     ecx,SCR_MAXCOLS
        jbe     mrn_room
        mov     ecx,SCR_MAXCOLS
mrn_room:
        mov     edi,OFFSET mcf_cells
        push    ecx
        rep     stosw
        pop     ecx

; mcf_cellsout - the ECX cells in mcf_cells at row DH, column DL; the
;                tail of mcf_put and mcf_run, inside their PUSHAD
mcf_cellsout:
        jecxz   mco_none
        movzx   ebx,dh
        imul    ebx,[mcf_cols]
        movzx   eax,dl
        add     ebx,eax
        mov     esi,OFFSET mcf_cells
        call    dword ptr ds:[IOS_PUTCELLS]
mco_none:
        popad
        ret

;=====================================================================
; Blocks, for the walks over the file (CONFIG.INC)
;=====================================================================

;---------------------------------------------------------------------
; mcf_header - a [block] line, ESI -> its '[' in cfg_line_buf.  Says
;              whether the lines after it count: a [COMMON] block's
;              always do, the chosen block's do, and no other's.
;
; The first walk, for SWITCHES=, reads every line wherever it is, as
; DOS's did - only notes that a block has begun, for NUMLOCK=.  Inside
; an INCLUDE the next [block] line is where the included block ends.
;---------------------------------------------------------------------
mcf_header:
        mov     byte ptr [mcf_seen],1
        cmp     dword ptr [cfg_pass],CFG_PASS_KEYS
        je      mhd_out
        cmp     dword ptr [mcf_depth],0
        jne     mhd_end
        inc     esi
        mov     edi,OFFSET mcfn_common
        call    mcf_same
        je      mhd_on
        mov     edi,[mcf_sel]
        or      edi,edi
        jz      mhd_off
        call    mcf_same
        je      mhd_on
mhd_off:
        mov     byte ptr [mcf_on],0
        ret
mhd_on:
        mov     byte ptr [mcf_on],1
mhd_out:
        ret
mhd_end:
        mov     byte ptr [mcf_stop],1
        ret

;---------------------------------------------------------------------
; INCLUDE=block - that block's lines, here, on every walk after the
;                 first.  ESI -> the value.
;
; Not [MENU] and not [COMMON], which are not configurations; not the
; chosen block, from inside itself; not a block already being included
; - any of those is a circle.  The walk of the block is cfg_lines,
; which comes back at the block's end (mcf_header) or the file's.
; Complaints are made once, on the main walk.
;---------------------------------------------------------------------
mcf_include:
        mov     edi,OFFSET mcfn_menu
        call    mcf_same
        je      min_bad
        mov     edi,OFFSET mcfn_common
        call    mcf_same
        je      min_bad
        call    mcf_find
        jc      min_none
        cmp     edi,[mcf_selblk]
        je      min_bad
        mov     ecx,[mcf_depth]
        cmp     ecx,MCF_INCMAX
        jae     min_bad
        xor     edx,edx
min_stk:
        cmp     edx,ecx
        jae     min_go
        cmp     edi,[mcf_incstk+edx*4]
        je      min_bad
        inc     edx
        jmp     short min_stk
min_go:
        mov     [mcf_incstk+ecx*4],edi
        inc     dword ptr [mcf_depth]
        mov     esi,edi
        call    cfg_lines
        mov     byte ptr [mcf_stop],0
        dec     dword ptr [mcf_depth]
        ret
min_none:
        mov     esi,OFFSET msgMcfNoBlk
        jmp     short min_say
min_bad:
        mov     esi,OFFSET msgCfgBadVal
min_say:
        cmp     dword ptr [cfg_pass],CFG_PASS_MAIN
        jne     min_out
        call    cfg_show_line
        call    dword ptr ds:[IOS_CONSTR]
min_out:
        ret

;---------------------------------------------------------------------
; mcf_numfirst - NUMLOCK= on the first walk.  Above the first block it
;                is acted on now, before the menu, whose arrow keys are
;                on the keypad - and the line becomes a remark, so that
;                the main walk neither does it again nor asks about it.
;                That is what DOS did with it.
;---------------------------------------------------------------------
mcf_numfirst:
        cmp     byte ptr [mcf_seen],0
        jne     mnf_out
        mov     esi,[cfg_val]
        call    cfg_numlock
        mov     edi,[cfg_lstart]
mnf_blank:
        cmp     byte ptr [edi],' '
        je      mnf_step
        cmp     byte ptr [edi],9
        jne     mnf_mark
mnf_step:
        inc     edi
        jmp     short mnf_blank
mnf_mark:
        mov     byte ptr [edi],';'
mnf_out:
        ret

;---------------------------------------------------------------------
; mcf_ask - EBX -> the cfg_table row of a statement about to be acted
;           on.  CF set if it is not to be: N was the answer, or F5
;           said N to everything left.
;
; Asked about when F8 asked for every line, or when the statement's
; name ended in '?'.  The question is the statement as DOS put it -
; the name in capitals, '=' (a space after SET), the value - and
; " [Y,N]?".  Esc answers Y for this line and every one after it.
;---------------------------------------------------------------------
mcf_ask:
        mov     eax,[ebx+4]
        cmp     eax,OFFSET cfg_rem      ; a remark has nothing to confirm,
        je      mak_yes
        cmp     eax,OFFSET cfg_menuonly ; nor a menu statement outside its
        je      mak_yes                 ; menu
        test    byte ptr [mcf_asks],CFGA_NO
        jnz     mak_no
        test    byte ptr [mcf_asks],CFGA_YES
        jnz     mak_yes
        test    byte ptr [mcf_asks],CFGA_EVERY
        jnz     mak_query
        cmp     byte ptr [cfg_qline],0
        je      mak_yes
mak_query:
        mov     esi,OFFSET cfg_key
        call    dword ptr ds:[IOS_CONSTR]
        mov     al,'='
        cmp     dword ptr [ebx+4],OFFSET cfg_set
        jne     mak_eq
        mov     al,' '
mak_eq:
        call    dword ptr ds:[IOS_CONOUT]
        mov     esi,[cfg_val]
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET msgCfgAsk
        call    dword ptr ds:[IOS_CONSTR]
        mov     al,1                    ; Esc may answer for the rest
        call    mcf_yesno
        cmp     al,'Y'
        je      mak_yes
mak_no:
        stc
        ret
mak_yes:
        clc
        ret

;---------------------------------------------------------------------
; mcf_yesno - wait for Y or N, show it, and a new line.  AL = 1 if Esc
;             may be the answer (Y, and to everything after).  F5 is
;             always one: N, and to everything after, AUTOEXEC.BAT
;             included.  -> AL = 'Y' or 'N'.  EBX is kept.
;---------------------------------------------------------------------
mcf_yesno:
        push    ebx
        push    esi
        mov     bl,al
myn_key:
        call    dword ptr ds:[IOS_CONIN]
        or      al,al
        jnz     myn_char
        cmp     ah,3Fh
        jne     myn_key
        or      byte ptr [mcf_asks],CFGA_NO
        mov     al,'N'
        jmp     short myn_say
myn_char:
        call    cfg_upcase
        cmp     al,'Y'
        je      myn_say
        cmp     al,'N'
        je      myn_say
        cmp     al,1Bh
        jne     myn_key
        or      bl,bl
        jz      myn_key
        or      byte ptr [mcf_asks],CFGA_YES
        mov     al,'Y'
myn_say:
        call    dword ptr ds:[IOS_CONOUT]
        mov     esi,OFFSET msgCrLf
        call    dword ptr ds:[IOS_CONSTR]
        pop     esi
        pop     ebx
        ret

;=====================================================================
; mcf_autoexec - the last word on AUTOEXEC.BAT, from DOSMAIN.ASM just
;                before the shell first starts.
;
; With F8 the question is asked, and a Y puts /Y on the shell's tail:
; CMD.EXE then shows each line of AUTOEXEC.BAT and runs it only on a Y
; (DOS 6.2's COMMAND /Y).  An N, or F5 at any question before it, puts
; /D there instead and the file is not run.  Esc, or a menu that timed
; out, leaves the tail as it is.
;=====================================================================
mcf_autoexec:
        test    byte ptr [mcf_asks],CFGA_NO
        jnz     mau_skip
        test    byte ptr [mcf_asks],CFGA_YES
        jnz     mau_out
        test    byte ptr [mcf_asks],CFGA_EVERY
        jz      mau_out
        mov     esi,OFFSET msgCfgAskAuto
        call    dword ptr ds:[IOS_CONSTR]
        xor     al,al                   ; not Esc: this question is the last
        call    mcf_yesno
        cmp     al,'Y'
        jne     mau_skip
        jmp     cfg_tailsw              ; AL = 'Y': /Y
mau_skip:
        mov     al,'D'
        jmp     cfg_tailsw
mau_out:
        ret

;=====================================================================
; Names
;=====================================================================

;---------------------------------------------------------------------
; mcf_find - ESI -> a block name.  EDI -> the line after its [name]
;            line in cfg_buf, CF clear; CF set if there is no such
;            block, or no name.  The first block of the name is the
;            one, as in DOS.
;---------------------------------------------------------------------
mcf_find:
        push    eax
        mov     al,[esi]
        call    mcf_isend
        je      mfd_none                ; no name is no block
        mov     edi,OFFSET cfg_buf
mfd_line:
        mov     al,[edi]
        cmp     al,' '
        je      mfd_blank
        cmp     al,9
        jne     mfd_look
mfd_blank:
        inc     edi
        jmp     short mfd_line
mfd_look:
        cmp     al,'['
        jne     mfd_next
        inc     edi
        call    mcf_same
        je      mfd_found
mfd_next:
        call    mcf_eol
        jnc     mfd_line
mfd_none:
        pop     eax
        stc
        ret
mfd_found:
        call    mcf_eol                 ; its statements start on the next
        pop     eax                     ; line - or at the end of the file,
        clc                             ; which is an empty block
        ret

;---------------------------------------------------------------------
; mcf_eol - EDI -> somewhere in a line of cfg_buf.  EDI -> the start of
;           the next line, CF clear; CF set at the end of the file (a
;           NUL or a Ctrl-Z, which EDI is left on).
;---------------------------------------------------------------------
mcf_eol:
        mov     al,[edi]
        or      al,al
        jz      mel_end
        cmp     al,1Ah
        je      mel_end
        inc     edi
        cmp     al,10
        je      mel_out
        cmp     al,13
        jne     mcf_eol
        cmp     byte ptr [edi],10
        jne     mel_out
        inc     edi
mel_out:
        clc
        ret
mel_end:
        stc
        ret

;---------------------------------------------------------------------
; mcf_same - ZF set if the names at ESI and EDI are the same, letters
;            in either case.  A name ends where mcf_isend says.  Both
;            pointers are kept.
;---------------------------------------------------------------------
mcf_same:
        push    eax
        push    ecx
        push    esi
        push    edi
msm_lp:
        mov     al,[esi]
        call    mcf_isend
        mov     ah,al
        mov     al,[edi]
        jne     msm_amore               ; A goes on
        call    mcf_isend               ; A ended: the same if B did too
        jmp     short msm_out
msm_amore:
        call    mcf_isend
        je      msm_no                  ; B ended and A did not
        call    cfg_upcase
        mov     cl,al
        mov     al,ah
        call    cfg_upcase
        cmp     al,cl
        jne     msm_out                 ; ZF clear
        inc     esi
        inc     edi
        jmp     short msm_lp
msm_no:
        or      esp,esp                 ; ZF clear: ESP is never zero
msm_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; mcf_isend - ZF set if AL ends a block name: the end of the line or
;             the file, a blank, a bracket, or = , ; / - DOS's list.
;
; The list is here and not with the data below because its length is
; an EQU, and an EQU used before wasm has seen it is an error.
;---------------------------------------------------------------------
mcf_ends        db  0,13,10,1Ah,' ',9,'[',']','=',',',';','/'
mcf_nends       EQU $ - mcf_ends

mcf_isend:
        push    ecx
        push    edi
        mov     edi,OFFSET mcf_ends
        mov     ecx,mcf_nends
        cld
        repne   scasb
        pop     edi
        pop     ecx
        ret

;---------------------------------------------------------------------
; mcf_skipname - ESI -> a name in cfg_line_buf.  ESI past it and past
;                what separates it from the text after it.
;---------------------------------------------------------------------
mcf_skipname:
        push    eax
msk_name:
        mov     al,[esi]
        call    mcf_isend
        je      msk_gap
        inc     esi
        jmp     short msk_name
msk_gap:
        mov     al,[esi]
        or      al,al
        jz      msk_out
        call    mcf_isend
        jne     msk_out
        inc     esi
        jmp     short msk_gap
msk_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; mcf_setenv - ESI -> a block name: CONFIG=name into the master
;              environment, as written in the MENUITEM line.  It goes
;              in before the main walk, so it is the environment's
;              first variable and a SET CONFIG= line can still change
;              it - both as DOS.
;---------------------------------------------------------------------
mcf_setenv:
        pushad
        mov     edi,OFFSET mcf_env
        mov     ebx,OFFSET mcfs_config
mse_pre:
        mov     al,[ebx]
        or      al,al
        jz      mse_name
        mov     [edi],al
        inc     ebx
        inc     edi
        jmp     short mse_pre
mse_name:
        mov     ecx,MCF_NAMEMAX
mse_lp:
        mov     al,[esi]
        call    mcf_isend
        je      mse_end
        mov     [edi],al
        inc     esi
        inc     edi
        loop    mse_lp
mse_end:
        mov     byte ptr [edi],0
        mov     esi,OFFSET mcf_env
        call    menv_set
        popad
        ret

;=====================================================================
; Data
;=====================================================================
mcfn_menu       db  'MENU',0
mcfn_common     db  'COMMON',0
mcfs_config     db  'CONFIG=',0

mcf_on          db  1               ; the lines being read are used
mcf_seen        db  0               ; a [block] line has gone by
mcf_stop        db  0               ; an included block has ended
mcf_typed       db  0               ; a key was pressed: no countdowns
mcf_asks        db  0               ; CFGA_*
mcf_said        db  0               ; a complaint since the screen cleared
mcf_ticking     db  0               ; the countdown is on the screen
mcf_attr        db  SCR_ATTR        ; MENUCOLOR=
mcf_sel         dd  0               ; -> the chosen block's name, or 0
mcf_selblk      dd  0               ; -> its first line
mcf_depth       dd  0               ; INCLUDEs inside one another
mcf_incstk      dd  MCF_INCMAX dup(0)
mcf_hops        dd  0
mcf_blk         dd  0               ; -> the menu being read
mcf_count       dd  0               ; its choices
mcf_dflt        dd  1               ; the default one, 1 up
mcf_cur         dd  1               ; the highlighted one
mcf_dfname      dd  0               ; -> MENUDEFAULT's name
mcf_tmo         dd  -1              ; seconds, -1 for none
mcf_t0          dd  0               ; the tick the countdown began on
mcf_shown       dd  -1              ; the seconds on the screen
mcf_name        dd  MCF_ITEMS dup(0)
mcf_desc        dd  MCF_ITEMS dup(0)
mcf_kind        db  MCF_ITEMS dup(0)
                db  0,0,0
mcf_rows        dd  SCR_ROWS
mcf_cols        dd  SCR_COLS
mcf_prow        dd  0               ; the prompt's row
mcf_pcol        dd  0               ; and where the digit goes on it
mcf_yncol       dd  0               ; the Y or N on the keys line
mcf_num         db  '0. ',0
mcf_nn          db  '00',0,0
mcf_env         db  8 + MCF_NAMEMAX dup(0)

msgMcfTitle     db  'PM-DOS Startup Menu',0
msgMcfPrompt    db  'Enter a choice: ',0
msgMcfTime      db  'Time remaining: ',0
msgMcfKeys      db  'F5=Skip CONFIG.SYS and AUTOEXEC.BAT  F8=Confirm each line [N]',0
msgMcfNoBlk     db  'Block not found in CONFIG.SYS',13,10,0
msgMcfAnyKey    db  'Press any key to continue . . .',0
msgCfgAsk       db  ' [Y,N]?',0
msgCfgAskAuto   db  'Process AUTOEXEC.BAT [Y,N]?',0

; A row of the menu screen, built and handed to IOS_PUTCELLS.  Below
; the arena and not in the file - see KBSS_BASE in PMDOS.INC.
_DOS    ENDS
_KBSS   SEGMENT
mcf_cells       dw  SCR_MAXCOLS dup(?)
IF ($ - kbss_start) AND 3
        .ERR    what follows mcf_cells in _KBSS is not on a dword
ENDIF
_KBSS   ENDS
_DOS    SEGMENT
