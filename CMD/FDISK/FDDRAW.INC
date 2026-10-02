;=====================================================================
; FDDRAW.INC - the screen.  Included by FDTUI.INC.
;
; Everything is drawn into scr_buf and nothing reaches the screen until
; scr_flush puts all of it there in one call, so a screen is never seen
; half drawn - and since the whole screen is drawn from the state every
; time, a box or a menu needs nothing saved from under it: it goes away
; by not being drawn the next time.
;
;   row  0      the title, with the close box in its corner
;   row  1      the menu bar
;   rows 2-9    the disk: the bar, the bracket under what is selected,
;               and a scale in megabytes
;   rows 10-23  the partitions as a list, and what the selected one is
;   row  24     the status line
;=====================================================================

;--- tui_draw - the whole screen, with an open menu over it ----------
tui_draw:
        call    tu_build
        call    mu_draw                 ; nothing when no menu is open
        call    scr_flush
        ret

;--- tu_build - everything but the menus and the boxes, into scr_buf -
tu_build:
        pushad
        mov     edi,OFFSET scr_buf
        mov     eax,(A_DESK SHL 8) OR ' '
        mov     ecx,TUI_CELLS
        rep     stosw
        mov     byte ptr [scr_cattr],A_DESK
        xor     ebx,ebx
        xor     ecx,ecx
        call    scr_at
        mov     edx,OFFSET msg_closebox
        call    scr_puts
        mov     edx,OFFSET msg_ttitle
        call    scr_centre
        call    mb_draw
        call    dp_draw
        call    ls_draw
        call    ip_draw
        call    st_draw
        popad
        ret

;--- mb_draw - the menu bar, the open menu's name lit ----------------
mb_draw:
        pushad
        mov     byte ptr [scr_cattr],A_MENU
        mov     ebx,ROW_MENU
        xor     ecx,ecx
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        xor     ebp,ebp
mbd_one:
        cmp     ebp,MU_COUNT
        jae     mbd_out
        imul    esi,ebp,MU_LEN
        add     esi,OFFSET mu_tbl
        mov     byte ptr [scr_cattr],A_MENU
        cmp     ebp,[mu_open]
        jne     mbd_attr
        mov     byte ptr [scr_cattr],A_MENUHI
mbd_attr:
        mov     ebx,ROW_MENU
        mov     ecx,[esi+MU_COL]
        call    scr_at
        mov     al,' '
        call    scr_putc
        mov     edx,[esi+MU_TITLE]
        call    scr_puts
        mov     al,' '
        call    scr_putc
        inc     ebp
        jmp     short mbd_one
mbd_out:
        popad
        ret

;=====================================================================
; dp_draw - the disk pane: its size in the border, the bar inside.
;=====================================================================
dp_draw:
        pushad
        mov     byte ptr [scr_cattr],A_PANE
        mov     ebx,DP_TOP
        mov     ecx,DP_LEFT
        mov     edx,DP_BOT
        mov     esi,DP_RIGHT
        xor     eax,eax
        call    scr_box
        mov     ebx,DP_TOP
        mov     ecx,DP_LEFT + 2
        call    scr_at
        mov     edx,OFFSET msg_pdisk
        call    scr_puts
        mov     eax,[tui_disk]
        mov     ebx,1
        call    scr_dec
        cmp     byte ptr [tui_dstate],1
        je      dpd_named
        mov     edx,OFFSET msg_dash
        call    scr_puts
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        mov     ebx,1
        call    scr_mb
        mov     edx,OFFSET msg_mbunit
        call    scr_puts
dpd_named:
        mov     al,' '
        call    scr_putc
        cmp     dword ptr [tui_ndisks],1
        jbe     dpd_inside
        mov     ebx,DP_TOP
        mov     ecx,DP_RIGHT - 18
        call    scr_at
        mov     edx,OFFSET msg_ptab
        call    scr_puts
dpd_inside:
        cmp     byte ptr [tui_dstate],0
        jne     dpd_state
        ; what the two shades mean
        mov     ebx,ROW_LEGEND
        mov     ecx,DP_RIGHT - 22
        call    scr_at
        mov     byte ptr [scr_cattr],A_FREE
        mov     al,GL_FREE
        call    scr_putc
        mov     byte ptr [scr_cattr],A_PANE
        mov     edx,OFFSET msg_lgfree
        call    scr_puts
        mov     byte ptr [scr_cattr],A_CONT
        mov     al,GL_CONT
        call    scr_putc
        mov     byte ptr [scr_cattr],A_PANE
        mov     edx,OFFSET msg_lgext
        call    scr_puts
        call    br_draw
        popad
        ret
dpd_state:
        ; no disk, no sector 0, or no table: which, where the bar would be
        mov     edx,OFFSET msg_tnodisk
        cmp     byte ptr [tui_dstate],1
        je      dpd_say
        mov     edx,OFFSET msg_tunread
        cmp     byte ptr [tui_dstate],2
        je      dpd_say
        mov     edx,OFFSET msg_tnotable
dpd_say:
        mov     ebx,ROW_LABEL
        call    scr_centre
        popad
        ret

;=====================================================================
; THE BAR.  A row of the table is a run of cells from start*W/total to
; end*W/total, three rows deep, and never fewer than one cell, so a tiny
; partition is a mark rather than nothing.  They are laid down in the
; order they lie on one another: free space, the container, the free
; space inside it, and the partitions with volumes over all of those -
; and bar_map remembers which row owns each cell, which is how a click
; on the bar finds what it was on.
;
; While a change is being considered (pv_mode) the selected partition's
; old place is drawn as the space it would leave and the change is drawn
; over everything, green where it fits and red where it does not.  For
; the extended partition the old place is everything in it, and the
; change is drawn shaded, with its logical drives where they would go.
;=====================================================================
br_draw:
        pushad
        mov     byte ptr [br_cxgone],0
        cmp     dword ptr [pv_mode],PV_REPLACE
        je      brd_cx
        cmp     dword ptr [pv_mode],PV_OMIT
        jne     brd_bar
brd_cx:
        cmp     dword ptr [tr_n],0
        je      brd_bar
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_CONTAINER
        jne     brd_bar
        mov     byte ptr [br_cxgone],1
brd_bar:
        mov     byte ptr [scr_cattr],A_FREE
        mov     ebx,ROW_BAR
brd_rows:
        mov     ecx,BAR_COL
        call    scr_at
        mov     al,GL_FREE
        mov     ecx,BAR_W
        call    scr_fill
        inc     ebx
        cmp     ebx,ROW_BAR + BAR_ROWS
        jb      brd_rows
        mov     edi,OFFSET bar_map
        mov     ecx,BAR_W
        mov     al,0FFh                 ; no cell is anybody's yet
        rep     stosb
        mov     eax,TK_FREE
        call    br_pass
        mov     eax,TK_CONTAINER
        call    br_pass
        mov     eax,TK_LFREE
        call    br_pass
        mov     eax,TK_PRIMARY          ; and the logical drives with them
        call    br_pass
        call    br_ghost
        call    br_mark
        call    br_scale
        popad
        ret

;--- br_pass - every row of kind EAX onto the bar --------------------
br_pass:
        pushad
        mov     [bp_kind],eax
        xor     ebp,ebp
brp_one:
        cmp     ebp,[tr_n]
        jae     brp_out
        imul    esi,ebp,TR_LEN
        add     esi,OFFSET tr_tbl
        mov     eax,[esi+TR_KIND]
        cmp     dword ptr [bp_kind],TK_PRIMARY
        jne     brp_exact
        cmp     eax,TK_LOGICAL
        je      brp_take
brp_exact:
        cmp     eax,[bp_kind]
        jne     brp_next
brp_take:
        mov     eax,[esi+TR_START]
        mov     ecx,[esi+TR_SECS]
        call    br_cells                ; EAX = first cell, ECX = cells
        push    eax
        push    ecx
        lea     edi,[bar_map+eax]
        mov     eax,ebp
        rep     stosb
        pop     ecx
        pop     eax
        mov     edx,[esi+TR_ATTR]       ; DL = its colour
        cmp     byte ptr [br_cxgone],0  ; the extended partition being moved:
        je      brp_kind                ; it and all it holds are the space
        cmp     dword ptr [esi+TR_KIND],TK_FREE     ; it would leave
        je      brp_kind
        cmp     dword ptr [esi+TR_KIND],TK_PRIMARY
        je      brp_kind
        mov     dl,A_FREE
        mov     dh,GL_FREE
        jmp     short brp_paint
brp_kind:
        mov     dh,GL_FREE
        cmp     dword ptr [esi+TR_KIND],TK_FREE
        je      brp_paint
        mov     dh,GL_CONT
        cmp     dword ptr [esi+TR_KIND],TK_CONTAINER
        je      brp_paint
        cmp     dword ptr [esi+TR_KIND],TK_LFREE
        je      brp_paint
        ; a partition: unless it is the one being moved or deleted, in
        ; which case what is drawn is the space it would leave behind
        mov     dh,' '
        cmp     ebp,[tr_sel]
        jne     brp_vol
        cmp     dword ptr [pv_mode],PV_REPLACE
        je      brp_gone
        cmp     dword ptr [pv_mode],PV_OMIT
        jne     brp_vol
brp_gone:
        mov     dl,A_FREE
        mov     dh,GL_FREE
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        jne     brp_paint
        mov     dl,A_CONT
        mov     dh,GL_CONT
brp_paint:
        call    br_block
        jmp     short brp_next
brp_vol:
        call    br_block
        call    br_label
brp_next:
        inc     ebp
        jmp     brp_one
brp_out:
        popad
        ret

;--- br_block - ECX cells from cell EAX, all three rows, in DL with DH -
br_block:
        pushad
        mov     esi,eax
        mov     [scr_cattr],dl
        mov     ebx,ROW_BAR
brb_row:
        push    ecx
        lea     ecx,[esi+BAR_COL]
        call    scr_at
        pop     ecx
        mov     al,dh
        call    scr_fill
        inc     ebx
        cmp     ebx,ROW_BAR + BAR_ROWS
        jb      brb_row
        popad
        ret

;---------------------------------------------------------------------
; br_label - ESI -> a row, EAX = its first cell, ECX = its cells, DL =
;            its colour.  Its number, and its drive letter if there is
;            room, in the middle of the block; yellow when it is the
;            one selected.  A block too narrow for the number goes
;            without.
;---------------------------------------------------------------------
br_label:
        pushad
        mov     [bl_first],eax
        mov     [bl_count],ecx
        mov     [bl_attr],dl
        mov     edi,OFFSET lb_text
        mov     eax,[esi+TR_NUM]
        call    tui_putdec
        mov     ecx,edi
        sub     ecx,OFFSET lb_text      ; ECX = the number alone
        cmp     dword ptr [esi+TR_LETTER],0
        je      brl_fit
        mov     edx,ecx
        mov     al,' '
        stosb
        mov     eax,[esi+TR_LETTER]
        stosb
        mov     al,':'
        stosb
        mov     ecx,edi
        sub     ecx,OFFSET lb_text      ; ECX = with the letter
        cmp     ecx,[bl_count]
        jbe     brl_fit
        mov     ecx,edx                 ; no room for the letter
brl_fit:
        cmp     ecx,[bl_count]
        ja      brl_out                 ; no room for anything
        mov     byte ptr [lb_text+ecx],'$'
        mov     eax,[bl_count]
        sub     eax,ecx
        shr     eax,1
        add     eax,[bl_first]
        lea     ecx,[eax+BAR_COL]
        mov     ebx,ROW_LABEL
        call    scr_at
        mov     al,[bl_attr]
        mov     edx,esi
        call    tui_selrow
        cmp     edx,esi
        jne     brl_attr
        and     al,0F0h
        or      al,0Eh
brl_attr:
        mov     [scr_cattr],al
        mov     edx,OFFSET lb_text
        call    scr_puts
brl_out:
        popad
        ret

;--- br_ghost - the change being considered, over everything ---------
br_ghost:
        pushad
        cmp     dword ptr [pv_mode],PV_REPLACE
        je      brg_show
        cmp     dword ptr [pv_mode],PV_ADD
        jne     brg_out
brg_show:
        mov     eax,[pv_start]
        mov     ecx,[pv_len]
        call    br_cells
        mov     dl,[pv_attr]
        mov     dh,' '
        cmp     byte ptr [br_cxgone],0
        je      brg_block
        mov     dh,GL_CONT              ; the extended partition, shaded
brg_block:
        call    br_block
        cmp     dword ptr [pv_mode],PV_ADD
        je      brg_new
        cmp     byte ptr [br_cxgone],0
        jne     brg_inside
        call    tui_selrow
        call    br_label
        jmp     short brg_out
brg_inside:
        call    br_cxin
        jmp     short brg_out
brg_new:
        ; a partition that does not exist yet: a plus where its number
        ; will be
        dec     ecx
        shr     ecx,1
        add     ecx,eax
        add     ecx,BAR_COL
        mov     ebx,ROW_LABEL
        call    scr_at
        mov     al,[pv_attr]
        and     al,0F0h
        or      al,0Eh
        mov     [scr_cattr],al
        mov     al,'+'
        call    scr_putc
brg_out:
        popad
        ret

;--- br_cxin - the logical drives, moved as far as the extended
;--- partition they are in is being moved: not at all, for a resize
br_cxin:
        pushad
        call    tui_selrow
        mov     eax,[pv_start]
        sub     eax,[esi+TR_START]
        mov     [br_shift],eax
        xor     ebp,ebp
bci_one:
        cmp     ebp,[tr_n]
        jae     bci_out
        imul    esi,ebp,TR_LEN
        add     esi,OFFSET tr_tbl
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        jne     bci_next
        mov     eax,[esi+TR_START]
        add     eax,[br_shift]
        mov     ecx,[esi+TR_SECS]
        call    br_cells
        mov     edx,[esi+TR_ATTR]
        mov     dh,' '
        call    br_block
        call    br_label
bci_next:
        inc     ebp
        jmp     short bci_one
bci_out:
        popad
        ret

;--- br_mark - a bracket under the selection, or under the change ----
br_mark:
        pushad
        mov     eax,[pv_start]
        mov     ecx,[pv_len]
        cmp     dword ptr [pv_mode],PV_REPLACE
        je      brm_have
        cmp     dword ptr [pv_mode],PV_ADD
        je      brm_have
        cmp     dword ptr [tr_n],0
        je      brm_out
        call    tui_selrow
        mov     eax,[esi+TR_START]
        mov     ecx,[esi+TR_SECS]
brm_have:
        call    br_cells
        mov     byte ptr [scr_cattr],A_PHEAD
        mov     ebx,ROW_MARK
        push    ecx
        lea     ecx,[eax+BAR_COL]
        call    scr_at
        pop     ecx
        cmp     ecx,1
        ja      brm_wide
        mov     al,18h                  ; an arrow, under a block of one cell
        call    scr_putc
        jmp     short brm_out
brm_wide:
        mov     al,0C0h
        call    scr_putc
        sub     ecx,2
        mov     al,0C4h
        call    scr_fill
        mov     al,0D9h
        call    scr_putc
brm_out:
        popad
        ret

;--- br_scale - nought, the middle and the end, in megabytes ---------
br_scale:
        pushad
        mov     byte ptr [scr_cattr],A_PANE
        mov     ebx,ROW_SCALE
        mov     ecx,BAR_COL
        call    scr_at
        mov     al,'0'
        call    scr_putc
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        shr     eax,1
        mov     ebx,1
        call    fmt_mb
        call    sd_len
        shr     ecx,1
        neg     ecx
        add     ecx,BAR_COL + BAR_W / 2
        mov     ebx,ROW_SCALE
        call    scr_at
        call    scr_puts
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        mov     ebx,1
        call    fmt_mb
        call    sd_len
        add     ecx,3                   ; and " MB"
        neg     ecx
        add     ecx,BAR_COL + BAR_W
        mov     ebx,ROW_SCALE
        call    scr_at
        call    scr_puts
        mov     edx,OFFSET msg_mbunit
        call    scr_puts
        popad
        ret

;---------------------------------------------------------------------
; br_cells - EAX = a start, ECX = sectors -> EAX = the first cell of the
;            bar it covers, ECX = how many cells (at least one, and
;            never past the end).
;---------------------------------------------------------------------
br_cells:
        push    ebx
        push    edx
        push    esi
        mov     ebx,dword ptr [dsk_rec+DSK_TOTAL]
        cmp     ebx,BAR_W
        jae     brc_tot
        mov     ebx,BAR_W               ; a disk smaller than the bar is not
brc_tot:                                ; one, but must not divide by nothing
        mov     esi,eax
        add     esi,ecx                 ; ESI = the end
        jnc     brc_end
        mov     esi,ebx
brc_end:
        cmp     eax,ebx
        jbe     brc_first
        mov     eax,ebx                 ; a start past the end of the disk
brc_first:
        cmp     esi,ebx
        jbe     brc_last
        mov     esi,ebx
brc_last:
        mov     ecx,BAR_W
        mul     ecx
        div     ebx
        push    eax                     ; the first cell
        mov     eax,esi
        mov     ecx,BAR_W
        mul     ecx
        div     ebx                     ; the cell it stops in front of
        pop     ecx                     ; ECX = the first
        cmp     ecx,BAR_W
        jb      brc_firstok
        mov     ecx,BAR_W - 1
brc_firstok:
        sub     eax,ecx
        jg      brc_len
        mov     eax,1
brc_len:
        xchg    eax,ecx                 ; EAX = first, ECX = cells
        lea     edx,[eax+ecx]
        cmp     edx,BAR_W
        jbe     brc_out
        mov     ecx,BAR_W
        sub     ecx,eax
brc_out:
        pop     esi
        pop     edx
        pop     ebx
        ret

;--- br_hit - EBX = row, ECX = column -> EAX = the table row whose
;--- block is there, or -1
br_hit:
        push    ecx
        cmp     byte ptr [tui_dstate],0
        jne     brh_none
        cmp     ebx,ROW_BAR
        jb      brh_none
        cmp     ebx,ROW_BAR + BAR_ROWS
        jae     brh_none
        sub     ecx,BAR_COL
        jb      brh_none
        cmp     ecx,BAR_W
        jae     brh_none
        movzx   eax,byte ptr [bar_map+ecx]
        cmp     al,0FFh
        je      brh_none
        pop     ecx
        ret
brh_none:
        mov     eax,-1
        pop     ecx
        ret

;=====================================================================
; ls_draw - the partitions as a list, in the order the bar has them.
;=====================================================================
ls_draw:
        pushad
        mov     byte ptr [scr_cattr],A_PANE
        mov     ebx,LP_TOP
        mov     ecx,LP_LEFT
        mov     edx,LP_BOT
        mov     esi,LP_RIGHT
        xor     eax,eax
        call    scr_box
        mov     ebx,LP_TOP
        mov     ecx,LP_LEFT + 2
        call    scr_at
        mov     edx,OFFSET msg_plist
        call    scr_puts
        mov     byte ptr [scr_cattr],A_PHEAD
        mov     ebx,ROW_LHEAD
        mov     ecx,LIST_COL
        call    scr_at
        mov     edx,OFFSET msg_lhead
        call    scr_puts
        ; the selection in view, and no rows wasted at the bottom
        mov     eax,[tr_sel]
        cmp     eax,[ls_top]
        jae     lsd_below
        mov     [ls_top],eax
lsd_below:
        sub     eax,LIST_ROWS - 1
        jle     lsd_fill
        cmp     eax,[ls_top]
        jbe     lsd_fill
        mov     [ls_top],eax
lsd_fill:
        mov     eax,[tr_n]
        sub     eax,LIST_ROWS
        jg      lsd_maxtop
        xor     eax,eax
lsd_maxtop:
        cmp     [ls_top],eax
        jbe     lsd_rows
        mov     [ls_top],eax
lsd_rows:
        xor     ebp,ebp
lsd_row:
        cmp     ebp,LIST_ROWS
        jae     lsd_scroll
        mov     eax,[ls_top]
        add     eax,ebp
        cmp     eax,[tr_n]
        jae     lsd_scroll
        imul    esi,eax,TR_LEN
        add     esi,OFFSET tr_tbl
        mov     dl,A_PANE
        cmp     dword ptr [esi+TR_KIND],TK_FREE
        je      lsd_dim
        cmp     dword ptr [esi+TR_KIND],TK_LFREE
        jne     lsd_sel
lsd_dim:
        mov     dl,A_PDIM
lsd_sel:
        cmp     eax,[tr_sel]
        jne     lsd_paint
        mov     dl,A_SEL
lsd_paint:
        mov     [scr_cattr],dl
        lea     ebx,[ebp+ROW_LFIRST]
        mov     ecx,LIST_COL
        call    scr_at
        mov     al,' '
        mov     ecx,LIST_W
        call    scr_fill
        call    ls_row
        inc     ebp
        jmp     short lsd_row
lsd_scroll:
        call    ls_scroll
        popad
        ret

;--- ls_row - ESI -> a row, on screen row EBX, in scr_cattr ----------
ls_row:
        pushad
        mov     dl,[scr_cattr]
        mov     [lr_attr],dl
        ; a swatch of its colour on the bar
        mov     ecx,LIST_COL
        call    scr_at
        mov     al,[esi+TR_ATTR]
        mov     [scr_cattr],al
        mov     eax,[esi+TR_KIND]
        mov     al,' '
        cmp     dword ptr [esi+TR_KIND],TK_FREE
        jne     lsr_cont
        mov     al,GL_FREE
lsr_cont:
        cmp     dword ptr [esi+TR_KIND],TK_CONTAINER
        je      lsr_shade
        cmp     dword ptr [esi+TR_KIND],TK_LFREE
        jne     lsr_swatch
lsr_shade:
        mov     al,GL_CONT
lsr_swatch:
        call    scr_putc
        mov     al,[lr_attr]
        mov     [scr_cattr],al
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_FREE
        je      lsr_free
        cmp     eax,TK_LFREE
        je      lsr_free
        mov     ecx,LSC_NUM
        call    scr_at
        mov     eax,[esi+TR_NUM]
        push    ebx
        mov     ebx,2
        call    scr_dec
        pop     ebx
        cmp     dword ptr [esi+TR_ACT],0
        je      lsr_drive
        mov     ecx,LSC_ACT
        call    scr_at
        mov     al,'A'
        call    scr_putc
lsr_drive:
        mov     eax,[esi+TR_LETTER]
        or      eax,eax
        jz      lsr_type
        mov     ecx,LSC_DRV
        call    scr_at
        call    scr_putc
        mov     al,':'
        call    scr_putc
lsr_type:
        mov     ecx,LSC_TYPE
        call    scr_at
        mov     eax,[esi+TR_TYPE]
        call    tn_find
        call    scr_puts
        jmp     short lsr_sizes
lsr_free:
        mov     ecx,LSC_TYPE
        call    scr_at
        mov     edx,OFFSET msg_lsfree
        cmp     eax,TK_FREE
        je      lsr_freeword
        mov     edx,OFFSET msg_lslfree
lsr_freeword:
        call    scr_puts
lsr_sizes:
        mov     ecx,LSC_SIZE
        call    scr_at
        mov     eax,[esi+TR_SECS]
        push    ebx
        mov     ebx,9
        call    scr_mb
        pop     ebx
        mov     ecx,LSC_START
        call    scr_at
        mov     eax,[esi+TR_START]
        push    ebx
        mov     ebx,9
        call    scr_mb
        pop     ebx
        popad
        ret

;--- ls_scroll - DiskStat's scroll bar, when the list is longer than
;--- its pane: arrows at the ends, the thumb where the view is
ls_scroll:
        pushad
        mov     eax,[tr_n]
        cmp     eax,LIST_ROWS
        jbe     lss_out
        mov     byte ptr [scr_cattr],A_PHEAD
        mov     ebx,LIST_ROWS - 2       ; the track
        imul    ebx,ebx,LIST_ROWS
        xchg    eax,ebx
        xor     edx,edx
        div     ebx                     ; thumb = track * rows / total
        or      eax,eax
        jnz     lss_some
        inc     eax
lss_some:
        mov     [ss_thumb],eax
        mov     ecx,LIST_ROWS - 2
        sub     ecx,eax                 ; how far it can travel
        mov     eax,[ls_top]
        mul     ecx
        mov     ecx,[tr_n]
        sub     ecx,LIST_ROWS
        div     ecx
        mov     [ss_pos],eax
        xor     ebp,ebp
lss_row:
        cmp     ebp,LIST_ROWS
        jae     lss_out
        lea     ebx,[ebp+ROW_LFIRST]
        mov     ecx,SCROLL_COL
        call    scr_at
        mov     al,18h
        or      ebp,ebp
        jz      lss_put
        mov     al,19h
        cmp     ebp,LIST_ROWS - 1
        je      lss_put
        mov     al,0B1h
        mov     edx,ebp
        dec     edx
        cmp     edx,[ss_pos]
        jb      lss_put
        sub     edx,[ss_pos]
        cmp     edx,[ss_thumb]
        jae     lss_put
        mov     al,0DBh
lss_put:
        call    scr_putc
        inc     ebp
        jmp     short lss_row
lss_out:
        popad
        ret

;--- ls_hit - EBX = row, ECX = column -> EAX = the table row there, or
;--- -1.  A press on the scroll bar moves the selection and is -1.
ls_hit:
        push    ecx
        cmp     ebx,ROW_LFIRST
        jb      lsh_none
        cmp     ebx,ROW_LFIRST + LIST_ROWS
        jae     lsh_none
        cmp     ecx,SCROLL_COL
        je      lsh_scroll
        cmp     ecx,LIST_COL
        jb      lsh_none
        cmp     ecx,LIST_COL + LIST_W
        jae     lsh_none
        mov     eax,ebx
        sub     eax,ROW_LFIRST
        add     eax,[ls_top]
        cmp     eax,[tr_n]
        jae     lsh_none
        pop     ecx
        ret
lsh_scroll:
        cmp     dword ptr [tr_n],LIST_ROWS
        jbe     lsh_none
        mov     eax,ebx
        sub     eax,ROW_LFIRST
        mov     ecx,-1                  ; the top arrow: a row up
        or      eax,eax
        jz      lsh_step
        mov     ecx,1                   ; the bottom one: a row down
        cmp     eax,LIST_ROWS - 1
        je      lsh_step
        mov     ecx,-LIST_ROWS          ; the track: a page
        cmp     eax,LIST_ROWS / 2
        jb      lsh_step
        mov     ecx,LIST_ROWS
lsh_step:
        call    tu_step
lsh_none:
        mov     eax,-1
        pop     ecx
        ret

;=====================================================================
; ip_draw - what the selection is, beside the list.
;=====================================================================
ip_draw:
        pushad
        mov     byte ptr [scr_cattr],A_PANE
        mov     ebx,LP_TOP
        mov     ecx,IP_LEFT
        mov     edx,LP_BOT
        mov     esi,IP_RIGHT
        xor     eax,eax
        call    scr_box
        cmp     byte ptr [tui_dstate],0
        jne     ipd_out
        cmp     dword ptr [tr_n],0
        je      ipd_out
        call    tui_selrow
        mov     dword ptr [ip_row],LP_TOP + 2
        ; its name, in the border
        mov     ebx,LP_TOP
        mov     ecx,IP_LEFT + 2
        call    scr_at
        mov     eax,[esi+TR_KIND]
        mov     edx,OFFSET msg_ifree
        cmp     eax,TK_FREE
        je      ipd_named
        cmp     eax,TK_LFREE
        je      ipd_named
        mov     edx,OFFSET msg_ilog
        cmp     eax,TK_LOGICAL
        je      ipd_number
        mov     edx,OFFSET msg_iext
        cmp     eax,TK_CONTAINER
        je      ipd_number
        mov     edx,OFFSET msg_ipart
ipd_number:
        call    scr_puts
        mov     eax,[esi+TR_NUM]
        push    ebx
        mov     ebx,1
        call    scr_dec
        pop     ebx
        mov     edx,OFFSET msg_sp1
ipd_named:
        call    scr_puts
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_FREE
        je      ipd_size
        cmp     eax,TK_LFREE
        je      ipd_size
        ; a partition: its letter and its type
        mov     eax,[esi+TR_LETTER]
        or      eax,eax
        jz      ipd_type
        mov     edx,OFFSET msg_idrive
        call    ip_line
        call    scr_putc
        mov     al,':'
        call    scr_putc
ipd_type:
        mov     edx,OFFSET msg_itype
        call    ip_line
        mov     eax,[esi+TR_TYPE]
        call    scr_hex2
        mov     edx,OFFSET msg_hexh
        call    scr_puts
        mov     eax,[esi+TR_TYPE]
        call    tn_find
        call    scr_puts
ipd_size:
        mov     edx,OFFSET msg_isize
        call    ip_line
        mov     eax,[esi+TR_SECS]
        call    ip_mb
        mov     edx,OFFSET msg_istart
        call    ip_line
        mov     eax,[esi+TR_START]
        call    ip_mb
        mov     edx,OFFSET msg_iend
        call    ip_line
        mov     eax,[esi+TR_START]
        add     eax,[esi+TR_SECS]
        call    ip_mb
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_PRIMARY
        jne     ipd_holds
        mov     edx,OFFSET msg_iactive
        call    ip_line
        mov     edx,OFFSET msg_ino
        cmp     dword ptr [esi+TR_ACT],0
        je      ipd_act
        mov     edx,OFFSET msg_iyes
ipd_act:
        call    scr_puts
        jmp     short ipd_hint
ipd_holds:
        cmp     eax,TK_CONTAINER
        jne     ipd_hint
        mov     edx,OFFSET msg_iholds
        call    ip_line
        xor     eax,eax                 ; how many logical drives
        xor     ecx,ecx
ipd_count:
        cmp     ecx,[tr_n]
        jae     ipd_counted
        imul    edx,ecx,TR_LEN
        cmp     dword ptr [tr_tbl+edx+TR_KIND],TK_LOGICAL
        jne     ipd_cnext
        inc     eax
ipd_cnext:
        inc     ecx
        jmp     short ipd_count
ipd_counted:
        push    ebx
        mov     ebx,1
        call    scr_dec
        pop     ebx
        mov     edx,OFFSET msg_ilogs
        call    scr_puts

        ; and two lines on what can be done with it
ipd_hint:
        mov     eax,[esi+TR_KIND]
        mov     edx,OFFSET msg_hvol
        cmp     eax,TK_PRIMARY
        je      ipd_vol
        cmp     eax,TK_LOGICAL
        jne     ipd_notvol
ipd_vol:
        call    tu_canmove
        jnc     ipd_say
        mov     edx,OFFSET msg_hnofat
        jmp     short ipd_say
ipd_notvol:
        mov     edx,OFFSET msg_hcont
        cmp     eax,TK_CONTAINER
        je      ipd_say
        cmp     eax,TK_LFREE
        je      ipd_lfree
        mov     edx,OFFSET msg_hfree
        cmp     dword ptr [tu_slot],0
        jne     ipd_say
        mov     edx,OFFSET msg_hnoslot
        jmp     short ipd_say
ipd_lfree:
        mov     edx,OFFSET msg_hlfree
        cmp     dword ptr [tu_lnum],0
        jne     ipd_say
        mov     edx,OFFSET msg_hnolink
ipd_say:
        mov     byte ptr [scr_cattr],A_PDIM
        mov     ebx,LP_BOT - 3
        mov     ecx,IP_TEXT
        call    scr_at
        call    scr_puts                ; the first line...
        call    sd_len
        lea     edx,[edx+ecx+1]         ; ...and the second, which follows it
        mov     ebx,LP_BOT - 2
        mov     ecx,IP_TEXT
        call    scr_at
        call    scr_puts
ipd_out:
        popad
        ret

;--- ip_line - EDX -> a label for the next line; the cursor is left
;--- where its value goes
ip_line:
        push    ebx
        push    ecx
        mov     ebx,[ip_row]
        mov     ecx,IP_TEXT
        call    scr_at
        call    scr_puts
        mov     ecx,IP_VALUE
        call    scr_at
        inc     dword ptr [ip_row]
        pop     ecx
        pop     ebx
        ret

ip_mb:                                  ; EAX sectors, as "40.8 MB"
        push    ebx
        push    edx
        mov     ebx,1
        call    scr_mb
        mov     edx,OFFSET msg_mbunit
        call    scr_puts
        pop     edx
        pop     ebx
        ret

;--- st_draw - the status line: a message, or the keys ---------------
st_draw:
        pushad
        mov     byte ptr [scr_cattr],A_STAT
        mov     ebx,ROW_STAT
        xor     ecx,ecx
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        mov     ebx,ROW_STAT
        mov     ecx,1
        call    scr_at
        mov     edx,[tu_stat]
        or      edx,edx
        jnz     std_say
        mov     edx,OFFSET msg_shints
        cmp     dword ptr [jn_n],0      ; with changes waiting, how many,
        je      std_say                 ; and the keys that commit and
        call    pd_hints                ; undo them
std_say:
        call    scr_puts
        popad
        ret

;=====================================================================
; The drawing primitives.  scr_at puts the cursor at row EBX, column
; ECX; everything after it writes there in scr_cattr and moves on.
;=====================================================================
scr_at:
        push    eax
        mov     eax,ebx
        imul    eax,TUI_COLS
        add     eax,ecx
        shl     eax,1
        add     eax,OFFSET scr_buf
        mov     [scr_pos],eax
        pop     eax
        ret

scr_putc:                               ; AL
        push    eax
        push    edi
        mov     edi,[scr_pos]
        cmp     edi,OFFSET scr_buf + TUI_CELLS * 2
        jae     spc_out
        mov     ah,[scr_cattr]
        mov     [edi],ax
        add     edi,2
        mov     [scr_pos],edi
spc_out:
        pop     edi
        pop     eax
        ret

scr_puts:                               ; EDX -> a '$' string
        push    eax
        push    esi
        mov     esi,edx
sps_one:
        mov     al,[esi]
        cmp     al,'$'
        je      sps_out
        call    scr_putc
        inc     esi
        jmp     short sps_one
sps_out:
        pop     esi
        pop     eax
        ret

scr_fill:                               ; AL, ECX times
        push    ecx
sf_one:
        or      ecx,ecx
        jz      sf_out
        call    scr_putc
        dec     ecx
        jmp     short sf_one
sf_out:
        pop     ecx
        ret

scr_dec:                                ; EAX, right-aligned in EBX
        push    edx
        call    fmt_n
        call    scr_puts
        pop     edx
        ret

scr_mb:                                 ; EAX sectors in megabytes, in EBX
        push    edx
        call    fmt_mb
        call    scr_puts
        pop     edx
        ret

scr_hex2:                               ; AL as two hex digits
        push    eax
        push    eax
        shr     al,4
        call    scr_hex1
        pop     eax
        call    scr_hex1
        pop     eax
        ret
scr_hex1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     sh1_put
        add     al,7
sh1_put:
        call    scr_putc
        ret

;--- scr_centre - EDX across the screen, centred, on row EBX ---------
scr_centre:
        push    eax
        push    ecx
        call    sd_len
        mov     eax,TUI_COLS
        sub     eax,ecx
        shr     eax,1
        mov     ecx,eax
        call    scr_at
        call    scr_puts
        pop     ecx
        pop     eax
        ret

;--- sd_len - EDX -> a '$' string -> ECX = its length ----------------
sd_len:
        xor     ecx,ecx
sdl_one:
        cmp     byte ptr [edx+ecx],'$'
        je      sdl_out
        inc     ecx
        jmp     short sdl_one
sdl_out:
        ret

;---------------------------------------------------------------------
; scr_box - a box, rows EBX to EDX and columns ECX to ESI, the inside
;           cleared; EAX = 0 for a single line, 1 for a double.  In
;           scr_cattr.
;---------------------------------------------------------------------
scr_box:
        pushad
        mov     [bx_top],ebx
        mov     [bx_left],ecx
        mov     [bx_bot],edx
        mov     [bx_right],esi
        mov     edi,OFFSET box_single
        or      eax,eax
        jz      sbx_glyphs
        mov     edi,OFFSET box_double
sbx_glyphs:
        mov     ebp,esi
        sub     ebp,ecx
        dec     ebp                     ; EBP = the inside width
        call    scr_at
        mov     al,[edi]
        call    scr_putc
        mov     ecx,ebp
        mov     al,[edi+4]
        call    scr_fill
        mov     al,[edi+1]
        call    scr_putc
        inc     ebx
sbx_mid:
        cmp     ebx,[bx_bot]
        jae     sbx_bottom
        mov     ecx,[bx_left]
        call    scr_at
        mov     al,[edi+5]
        call    scr_putc
        mov     ecx,ebp
        mov     al,' '
        call    scr_fill
        mov     al,[edi+5]
        call    scr_putc
        inc     ebx
        jmp     short sbx_mid
sbx_bottom:
        mov     ecx,[bx_left]
        call    scr_at
        mov     al,[edi+2]
        call    scr_putc
        mov     ecx,ebp
        mov     al,[edi+4]
        call    scr_fill
        mov     al,[edi+3]
        call    scr_putc
        popad
        ret

;--- scr_shade - the shadow of that box: two columns to its right and a
;--- row under it, a cell over - what DOSForms draws
scr_shade:
        pushad
        mov     [bx_left],ecx
        mov     [bx_bot],edx
        mov     [bx_right],esi
        inc     ebx
shd_side:
        cmp     ebx,[bx_bot]
        ja      shd_under
        mov     ecx,[bx_right]
        inc     ecx
        call    shade_cell
        inc     ecx
        call    shade_cell
        inc     ebx
        jmp     short shd_side
shd_under:
        mov     ecx,[bx_left]
        add     ecx,2
shd_ucol:
        mov     eax,[bx_right]
        add     eax,2
        cmp     ecx,eax
        ja      shd_out
        call    shade_cell
        inc     ecx
        jmp     short shd_ucol
shd_out:
        popad
        ret

shade_cell:                             ; row EBX, column ECX
        cmp     ebx,TUI_ROWS
        jae     shc_out
        cmp     ecx,TUI_COLS
        jae     shc_out
        push    eax
        mov     eax,ebx
        imul    eax,TUI_COLS
        add     eax,ecx
        mov     byte ptr [scr_buf+eax*2+1],A_SHADOW
        pop     eax
shc_out:
        ret

scr_flush:
        pushad
        mov     esi,OFFSET scr_buf
        xor     ebx,ebx
        mov     ecx,TUI_CELLS
        mov     eax,0F004h
        int     21h
        popad
        ret

;---------------------------------------------------------------------
; fmt_mb - EAX sectors as megabytes to a tenth, right-aligned in EBX, as
;          EDX -> a '$' string in mb_buf.  Rounded to the nearest tenth;
;          from 10000 MB up, to the nearest megabyte.
;---------------------------------------------------------------------
fmt_mb:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     ecx,10
        mul     ecx
        add     eax,1024
        adc     edx,0
        mov     ecx,2048
        div     ecx                     ; EAX = tenths of a megabyte
        mov     edi,OFFSET mb_buf + 23
        mov     byte ptr [edi],'$'
        xor     esi,esi                 ; ESI = characters so far
        mov     ecx,10
        cmp     eax,100000
        jb      fmb_tenth
        add     eax,5
        xor     edx,edx
        div     ecx
        jmp     short fmb_whole
fmb_tenth:
        xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        dec     edi
        mov     byte ptr [edi],'.'
        add     esi,2
fmb_whole:
        xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        inc     esi
        or      eax,eax
        jnz     fmb_whole
fmb_pad:
        cmp     esi,ebx
        jae     fmb_out
        dec     edi
        mov     byte ptr [edi],' '
        inc     esi
        jmp     short fmb_pad
fmb_out:
        mov     edx,edi
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; Data for the drawing.
;=====================================================================
scr_pos     dd  0
bp_kind     dd  0
bl_first    dd  0
bl_count    dd  0
br_shift    dd  0                       ; how far a moved container's drives go
ip_row      dd  0
ss_thumb    dd  0
ss_pos      dd  0
bx_top      dd  0
bx_left     dd  0
bx_bot      dd  0
bx_right    dd  0
scr_cattr   db  A_PANE
bl_attr     db  0
lr_attr     db  0
br_cxgone   db  0                       ; the container is what is changing
bar_map     db  BAR_W + 2 dup(0)
lb_text     db  16 dup(0)
mb_buf      db  24 dup(0)
box_single  db  0DAh,0BFh,0C0h,0D9h,0C4h,0B3h
box_double  db  0C9h,0BBh,0C8h,0BCh,0CDh,0BAh

msg_pdisk   db  ' Disk $'
msg_dash    db  ' - $'
msg_mbunit  db  ' MB$'
msg_sp1     db  ' $'
msg_hexh    db  'h $'
msg_ptab    db  ' Tab: next disk $'
msg_lgfree  db  ' free  $'
msg_lgext   db  ' extended$'
msg_plist   db  ' Partitions $'
msg_lhead   db  '  # A Drv Type        Size MB  Start MB$'
msg_lsfree  db  'free$'
msg_lslfree db  'free (ext)$'
msg_ipart   db  ' Partition $'
msg_ilog    db  ' Logical drive $'
msg_iext    db  ' Extended partition $'
msg_ifree   db  ' Free space $'
msg_idrive  db  'Drive$'
msg_itype   db  'Type$'
msg_isize   db  'Size$'
msg_istart  db  'Starts at$'
msg_iend    db  'Ends at$'
msg_iactive db  'Active$'
msg_iholds  db  'Holds$'
msg_iyes    db  'yes$'
msg_ino     db  'no$'
msg_ilogs   db  ' logical$'
; two lines each, the second straight after the first
msg_hvol    db  'Drag a partition to$','move it.  S to resize.$'
msg_hnofat  db  'FDISK cannot move or$','resize non-FAT drives.$'
msg_hcont   db  'Logical drives are$','the blocks inside it.$'
msg_hfree   db  'Press C to create a$','partition here.$'
msg_hnoslot db  'All four primary slots$','are in use.$'
msg_hlfree  db  'Press C to create a$','logical drive here.$'
msg_hnolink db  'No more logical drives$','can go here.$'
