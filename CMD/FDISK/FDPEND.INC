;=====================================================================
; FDPEND.INC - CHANGES THAT WAIT: the screen's Commit and Undo.
;              Included by FDTUI.INC.
;
; On the screen nothing is written when a change is asked for.  It is
; STAGED: the verb runs exactly as the command line would run it, with
; dk_armed at DK_STAGE, and every sector it writes goes into the
; overlay below instead of to the disk.  dk_read lays the overlay over
; whatever it reads, so from then on every verb - the next change's
; dry run, the table the bar is drawn from - sees the disk as it will
; be.  The change goes into the journal: the verb and its arguments,
; as the command line would have been given them, and the question the
; screen asked about it.
;
; W COMMITS.  The overlay is thrown away and the journal run again, in
; order, with the writes on.  Each verb plans afresh against the real
; disk, which by then is what the one before it left - the state it was
; staged against.  U UNDOES the last change: the journal loses its last
; entry and the overlay is rebuilt by staging the rest again.
;
; A MOVE OR A RESIZE STAGES ITS TABLE ENTRY AND NOTHING ELSE.  The data
; it would move is far too much to hold, and nothing on the screen
; reads it - but a second change to the SAME volume would plan against
; a boot sector and a FAT that are still where they were.  So the
; volume is LOCKED until the commit: no second move or resize and no
; details until W or U.  A container move carries its records and its
; volumes' boot sectors into the overlay, because the chain is what the
; screen draws; the drives in it are locked all the same.
;
; FORMAT IS NOT STAGED.  It writes a volume, not a table, and only when
; nothing is waiting - so what it formats is what is on the disk.
;=====================================================================

;--- one change waiting, as jn_tbl holds it --------------------------
JN_VERB         EQU     00h             ; dd - the verb: fd_change, ...
JN_CMD          EQU     04h             ; dd - fd_cmd, which is a byte
JN_NARGS        EQU     08h             ; dd - fd_nargs
JN_SW           EQU     0Ch             ; dd - fd_sw
JN_GPWANT       EQU     10h             ; dd - gp_want: the free run a create
                                        ;      meant, which is not an argument
JN_DRIVE        EQU     14h             ; dd - the BIOS drive it is on
JN_LOCK0        EQU     18h             ; dd - the range whose volumes it leaves
JN_LOCK1        EQU     1Ch             ; dd   unreadable until the commit,
                                        ;      0..0 when it leaves none
JN_UNLOCK       EQU     20h             ; dd - the start of what a delete took,
                                        ;      or -1
JN_FLAGS        EQU     24h             ; dd - JF_*
JN_ARGS         EQU     28h             ; fd_args, all of it
JN_TEXT         EQU     JN_ARGS + FD_MAXARGS * FD_ARGLEN
                                        ; and the question it was, '$' ended
JN_TEXTLEN      EQU     DG_LINEW + 1
IF JN_TEXT + JN_TEXTLEN GT JN_LEN
        .ERR    JN_LEN in FDISK.ASM is too small for a journal entry
ENDIF
JF_SLOW         EQU     1               ; it moves data: the commit takes time

PD_SHOWN        EQU     5               ; changes the Commit box lists

;=====================================================================
; THE OVERLAY.  Sectors staged, each with its drive and LBA; a sector
; staged twice is kept once, as it was last written.
;=====================================================================
ov_clear:
        mov     dword ptr [ov_n],0
        ret

;--- ov_find - AL = a drive, EBX = an LBA -> CF clear, EDX = its slot -
ov_find:
        xor     edx,edx
ovf_one:
        cmp     edx,[ov_n]
        jae     ovf_none
        cmp     ebx,[ov_lba+edx*4]
        jne     ovf_next
        cmp     al,[ov_drv+edx]
        je      ovf_got
ovf_next:
        inc     edx
        jmp     short ovf_one
ovf_none:
        stc
        ret
ovf_got:
        clc
        ret

;--- ov_put - AL = a drive, EBX = an LBA, ESI -> the sector: staged.
;--- CF when the overlay is full, and ov_full says so to the screen.
ov_put:
        pushad
        call    ov_find
        jnc     ovp_have
        mov     edx,[ov_n]
        cmp     edx,OV_MAX
        jae     ovp_full
        inc     dword ptr [ov_n]
        mov     [ov_lba+edx*4],ebx
        mov     [ov_drv+edx],al
ovp_have:
        mov     edi,edx
        shl     edi,9                   ; a sector is 512 bytes
        add     edi,OFFSET ov_data
        mov     ecx,SEC_BYTES / 4
        rep     movsd
        popad
        clc
        ret
ovp_full:
        mov     byte ptr [ov_full],1
        popad
        stc
        ret

;--- ov_patch - AL = a drive, EBX = the first LBA, ECX = sectors, EDI ->
;--- what dk_read has just read: every staged sector in the range laid
;--- over it
ov_patch:
        pushad
        mov     [ovt_drv],al
        mov     [ovt_lba],ebx
        mov     [ovt_cnt],ecx
        mov     [ovt_buf],edi
        xor     edx,edx
ovt_one:
        cmp     edx,[ov_n]
        jae     ovt_out
        mov     al,[ovt_drv]
        cmp     al,[ov_drv+edx]
        jne     ovt_next
        mov     eax,[ov_lba+edx*4]
        sub     eax,[ovt_lba]
        jb      ovt_next                ; in front of the range
        cmp     eax,[ovt_cnt]
        jae     ovt_next                ; past it
        shl     eax,9
        mov     edi,[ovt_buf]
        add     edi,eax
        mov     esi,edx
        shl     esi,9
        add     esi,OFFSET ov_data
        mov     ecx,SEC_BYTES / 4
        rep     movsd
ovt_next:
        inc     edx
        jmp     short ovt_one
ovt_out:
        popad
        ret

;=====================================================================
; THE JOURNAL.
;=====================================================================

;--- jn_ptr - EBP = an index -> ESI -> that entry ---------------------
jn_ptr:
        imul    esi,ebp,JN_LEN
        add     esi,OFFSET jn_tbl
        ret

;--- jn_load - EBP = an index: the command line it was, put back.
;--- -> EAX = its verb
jn_load:
        push    ecx
        push    esi
        push    edi
        call    jn_ptr
        mov     eax,[esi+JN_CMD]
        mov     [fd_cmd],al
        mov     eax,[esi+JN_NARGS]
        mov     [fd_nargs],eax
        mov     eax,[esi+JN_SW]
        mov     [fd_sw],eax
        mov     eax,[esi+JN_GPWANT]
        mov     [gp_want],eax
        push    esi
        add     esi,JN_ARGS
        mov     edi,OFFSET fd_args
        mov     ecx,FD_MAXARGS * FD_ARGLEN / 4
        rep     movsd
        pop     esi
        mov     dword ptr [pl_status],-1
        mov     eax,[esi+JN_VERB]
        pop     edi
        pop     esi
        pop     ecx
        ret

;--- jn_settext - ESI -> an entry: the first line of the box just
;--- answered, without its question mark, is what the change is called
jn_settext:
        pushad
        lea     edi,[esi+JN_TEXT]
        mov     edx,edi                 ; EDX = where it starts
        cmp     dword ptr [dg_n],0
        je      jst_term
        mov     esi,[dg_lines]
        mov     ecx,DG_LINEW
jst_ch:
        mov     al,[esi]
        cmp     al,'$'
        je      jst_end
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     jst_ch
jst_end:
        cmp     edi,edx
        je      jst_term
        cmp     byte ptr [edi-1],'?'
        jne     jst_term
        dec     edi
jst_term:
        mov     byte ptr [edi],'$'
        popad
        ret

;--- jn_setlock - ESI -> the entry just staged: the range it leaves
;--- unreadable until the commit, and what it takes away
jn_setlock:
        push    eax
        xor     eax,eax
        mov     [esi+JN_LOCK0],eax
        mov     [esi+JN_LOCK1],eax
        mov     [esi+JN_FLAGS],eax
        dec     eax
        mov     [esi+JN_UNLOCK],eax
        movzx   eax,byte ptr [pl_drive]
        mov     [esi+JN_DRIVE],eax
        mov     eax,[esi+JN_VERB]
        cmp     eax,OFFSET fd_change
        je      jsl_change
        cmp     eax,OFFSET fd_delete
        jne     jsl_out
        mov     eax,[rs_start]          ; the start of what was deleted, as
        mov     [esi+JN_UNLOCK],eax     ; the screen named it
        jmp     short jsl_out
jsl_change:
        cmp     byte ptr [pl_raw],0
        jne     jsl_out                 ; never formatted: nothing in it to be
                                        ; out of date
        cmp     byte ptr [pl_cont],0
        je      jsl_lock
        mov     eax,[pl_nstart]
        cmp     eax,[pl_ostart]
        je      jsl_out                 ; a container resized moves nothing
jsl_lock:
        or      dword ptr [esi+JN_FLAGS],JF_SLOW
        mov     eax,[pl_nstart]
        mov     [esi+JN_LOCK0],eax
        add     eax,[pl_nlen]
        mov     [esi+JN_LOCK1],eax
jsl_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; pd_locked - EAX = a partition's start, DL = its BIOS drive.  CF set
;             when a change waiting has left it unreadable: inside the
;             range a move or a resize locked, unless a later delete
;             took what was locked.
;---------------------------------------------------------------------
pd_locked:
        pushad
        mov     [pdl_start],eax
        movzx   eax,dl
        mov     [pdl_drive],eax
        xor     ebp,ebp
pdl_one:
        cmp     ebp,[jn_n]
        jae     pdl_no
        call    jn_ptr
        mov     eax,[pdl_drive]
        cmp     eax,[esi+JN_DRIVE]
        jne     pdl_next
        cmp     dword ptr [esi+JN_LOCK1],0
        je      pdl_next
        mov     eax,[pdl_start]
        cmp     eax,[esi+JN_LOCK0]
        jb      pdl_next
        cmp     eax,[esi+JN_LOCK1]
        jae     pdl_next
        mov     ebx,[esi+JN_LOCK0]      ; covered: has anything since
        lea     ecx,[ebp+1]             ; deleted what was locked?
pdl_later:
        cmp     ecx,[jn_n]
        jae     pdl_yes
        imul    edi,ecx,JN_LEN
        add     edi,OFFSET jn_tbl
        mov     eax,[pdl_drive]
        cmp     eax,[edi+JN_DRIVE]
        jne     pdl_lnext
        cmp     ebx,[edi+JN_UNLOCK]
        je      pdl_next                ; yes: this lock is over
pdl_lnext:
        inc     ecx
        jmp     short pdl_later
pdl_next:
        inc     ebp
        jmp     short pdl_one
pdl_no:
        popad
        clc
        ret
pdl_yes:
        popad
        stc
        ret

;=====================================================================
; tu_stage - EAX = a verb, with fd_args and the rest set as for
;            tu_write: the change is staged and added to what is
;            waiting.  What the box just asked is what it is called.
;=====================================================================
tu_stage:
        pushad
        mov     [wr_verb],eax
        mov     ebp,[jn_n]
        cmp     ebp,JN_MAX
        jae     tsg_full
        call    jn_ptr                  ; ESI -> the entry
        mov     [esi+JN_VERB],eax
        movzx   eax,byte ptr [fd_cmd]
        mov     [esi+JN_CMD],eax
        mov     eax,[fd_nargs]
        mov     [esi+JN_NARGS],eax
        mov     eax,[fd_sw]
        mov     [esi+JN_SW],eax
        mov     eax,[gp_want]
        mov     [esi+JN_GPWANT],eax
        push    esi
        lea     edi,[esi+JN_ARGS]
        mov     esi,OFFSET fd_args
        mov     ecx,FD_MAXARGS * FD_ARGLEN / 4
        rep     movsd
        pop     esi
        call    jn_settext

        mov     byte ptr [ov_full],0
        movzx   eax,byte ptr [dk_armed]
        push    eax
        mov     byte ptr [dk_armed],DK_STAGE
        mov     eax,[wr_verb]
        call    tui_run                 ; every register but ESP is gone
        pop     eax
        mov     [dk_armed],al
        cmp     byte ptr [fd_rc],0
        jne     tsg_failed
        mov     ebp,[jn_n]
        call    jn_ptr
        call    jn_setlock
        inc     dword ptr [jn_n]
        mov     dword ptr [cap_len],0   ; what staging printed is no change's
        jmp     short tsg_show          ; log: the commit writes that

tsg_failed:
        ; WHAT IT STAGED BEFORE IT STOPPED GOES, by staging the rest again
        call    dg_reset
        cmp     byte ptr [ov_full],0
        jne     tsg_toomany
        mov     ecx,6
        call    ln_captail
        jmp     short tsg_say
tsg_toomany:
        call    ln_new
        mov     edx,OFFSET msg_xpending
        call    lb_puts
tsg_say:
        mov     edx,OFFSET msg_tfailed
        mov     bl,A_DLGERR
        call    tu_box
        call    pd_restage
tsg_show:
        mov     dword ptr [pv_mode],PV_NONE
        call    tui_load
        call    tu_reselect
        popad
        ret
tsg_full:
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_xpending
        call    lb_puts
        mov     edx,OFFSET msg_tfailed
        mov     bl,A_DLGERR
        call    tu_box
        popad
        ret

;---------------------------------------------------------------------
; pd_restage - the overlay built again from the journal.  An entry that
;              will not stage now - which nothing should make happen -
;              is dropped with every one after it, and the box says so.
;---------------------------------------------------------------------
pd_restage:
        pushad
        movzx   eax,byte ptr [dk_armed]
        push    eax
prs_again:
        call    ov_clear
        mov     dword ptr [prs_i],0
prs_one:
        mov     ebp,[prs_i]
        cmp     ebp,[jn_n]
        jae     prs_done
        call    jn_load
        mov     byte ptr [dk_armed],DK_STAGE
        call    tui_run
        cmp     byte ptr [fd_rc],0
        jne     prs_lost
        inc     dword ptr [prs_i]
        jmp     short prs_one
prs_lost:
        mov     eax,[prs_i]
        mov     [jn_n],eax              ; this one and every one after it
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_xdropped
        call    lb_puts
        call    ln_new
        mov     edx,OFFSET msg_xdropped2
        call    lb_puts
        mov     edx,OFFSET msg_tfailed
        mov     bl,A_DLGERR
        call    tu_box
        jmp     short prs_again
prs_done:
        pop     eax
        mov     [dk_armed],al
        mov     dword ptr [gp_want],0
        mov     dword ptr [cap_len],0
        popad
        ret

;=====================================================================
; U - UNDO the last change waiting.  It is named on the status line.
;=====================================================================
tu_undo:
        pushad
        mov     ebp,[jn_n]
        or      ebp,ebp
        jnz     tud_some
        mov     dword ptr [tu_stat],OFFSET msg_tnoundo
        jmp     short tud_out
tud_some:
        dec     ebp
        mov     [jn_n],ebp
        mov     edi,OFFSET st_buf
        mov     ecx,TUI_COLS - 2
        call    lb_into
        mov     edx,OFFSET msg_tundone
        call    lb_puts
        call    jn_ptr
        lea     edx,[esi+JN_TEXT]
        call    lb_puts
        call    pd_restage
        mov     dword ptr [pv_mode],PV_NONE
        call    tui_load
        mov     dword ptr [tu_stat],OFFSET st_buf
tud_out:
        popad
        ret

;=====================================================================
; W - COMMIT what is waiting, after a box that lists it.
;=====================================================================
tu_commit:
        pushad
        cmp     dword ptr [jn_n],0
        jne     tcm_some
        mov     dword ptr [tu_stat],OFFSET msg_tnocommit
        jmp     short tcm_out
tcm_some:
        call    pd_askcommit
        jc      tcm_out
        call    pd_commit
tcm_out:
        popad
        ret

;--- pd_askcommit - the box: what is waiting, and whether to write it.
;--- CF clear on Yes.
pd_askcommit:
        pushad
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_qcommit
        call    lb_puts
        call    pd_multi
        mov     ebx,[jn_n]              ; EBX = how many get a line
        cmp     ebx,PD_SHOWN
        jbe     pac_lines
        mov     ebx,PD_SHOWN - 1        ; and one says how many more
pac_lines:
        xor     ebp,ebp
pac_one:
        cmp     ebp,ebx
        jae     pac_more
        call    ln_new
        call    pd_name
        inc     ebp
        jmp     short pac_one
pac_more:
        mov     eax,[jn_n]
        sub     eax,ebx
        jz      pac_scan
        call    ln_new
        mov     edx,OFFSET msg_qcmore
        call    lb_puts
        call    lb_num
        mov     edx,OFFSET msg_qcmore2
        call    lb_puts
pac_scan:
        xor     ebp,ebp                 ; anything that moves data?
pac_slow:
        cmp     ebp,[jn_n]
        jae     pac_ask
        call    jn_ptr
        test    dword ptr [esi+JN_FLAGS],JF_SLOW
        jnz     pac_warn
        inc     ebp
        jmp     short pac_slow
pac_warn:
        call    ln_new
        mov     edx,OFFSET msg_qrmwarn
        call    lb_puts
pac_ask:
        mov     edx,OFFSET msg_tcommit
        mov     bl,A_DLG
        xor     ecx,ecx                 ; Enter is Yes: W asked for this
        call    tu_ask
        popad
        ret

;--- pd_multi - pd_multi_on = 1 when what is waiting is on more than
;--- one disk, and each line then says which
pd_multi:
        pushad
        mov     byte ptr [pd_multi_on],0
        xor     ebp,ebp
        call    jn_ptr
        mov     eax,[esi+JN_DRIVE]
pdm_one:
        cmp     ebp,[jn_n]
        jae     pdm_out
        call    jn_ptr
        cmp     eax,[esi+JN_DRIVE]
        jne     pdm_two
        inc     ebp
        jmp     short pdm_one
pdm_two:
        mov     byte ptr [pd_multi_on],1
pdm_out:
        popad
        ret

;--- pd_name - EBP = an index: what that change is called, onto the
;--- line being built
pd_name:
        push    eax
        push    edx
        push    esi
        call    jn_ptr
        cmp     byte ptr [pd_multi_on],0
        je      pdn_text
        mov     edx,OFFSET msg_qcdisk
        call    lb_puts
        mov     eax,[esi+JN_DRIVE]
        sub     eax,80h
        call    lb_num
        mov     edx,OFFSET msg_qcdisk2
        call    lb_puts
pdn_text:
        lea     edx,[esi+JN_TEXT]
        call    lb_puts
        pop     esi
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; pd_commit - what is waiting, written.  The overlay goes first, so
;             that every verb reads the disk itself; each change is
;             then run for real in the order it was asked for.  One
;             that fails stops the rest: they were planned on top of
;             it.  Either way nothing is waiting afterwards.
;---------------------------------------------------------------------
pd_commit:
        pushad
        call    ov_clear
        mov     dword ptr [cap_len],0
        mov     dword ptr [pdc_i],0
        mov     byte ptr [pdc_bad],0
pdc_one:
        mov     ebp,[pdc_i]
        cmp     ebp,[jn_n]
        jae     pdc_done
        ; "Writing change 2 of 3..."
        mov     edi,OFFSET st_buf
        mov     ecx,TUI_COLS - 2
        call    lb_into
        mov     edx,OFFSET msg_tcwrite
        call    lb_puts
        lea     eax,[ebp+1]
        call    lb_num
        mov     edx,OFFSET msg_tcof
        call    lb_puts
        mov     eax,[jn_n]
        call    lb_num
        mov     edx,OFFSET msg_tcdots
        call    lb_puts
        mov     dword ptr [tu_stat],OFFSET st_buf
        call    tui_draw
        mov     ebp,[pdc_i]
        call    jn_load
        mov     [wr_verb],eax
        mov     byte ptr [dk_armed],1
        mov     byte ptr [dk_wrote],0
        call    tui_run
        mov     byte ptr [dk_armed],1
        ; WHAT THE RUNNING SYSTEM CANNOT SEE, as tu_write decides it: a
        ; partition made, removed, moved or resized, finished or not
        cmp     byte ptr [dk_wrote],0
        je      pdc_seen
        cmp     dword ptr [wr_verb],OFFSET fd_active
        je      pdc_seen
        cmp     dword ptr [wr_verb],OFFSET fd_mbr
        je      pdc_seen
        mov     byte ptr [tu_changed],1
pdc_seen:
        cmp     byte ptr [fd_rc],0
        jne     pdc_failed
        inc     dword ptr [pdc_i]
        jmp     pdc_one
pdc_failed:
        mov     byte ptr [pdc_bad],1
pdc_done:
        mov     eax,[jn_n]
        mov     [pdc_n],eax
        mov     dword ptr [jn_n],0
        mov     dword ptr [gp_want],0
        mov     dword ptr [tu_stat],0
        mov     dword ptr [pv_mode],PV_NONE
        call    tui_load
        call    dg_reset
        cmp     byte ptr [pdc_bad],0
        jne     pdc_saybad
        call    ln_new
        mov     edx,OFFSET msg_xwritten
        call    lb_puts
        cmp     byte ptr [tu_changed],0
        je      pdc_good
        call    ln_new
        mov     edx,OFFSET msg_xrestart
        call    lb_puts
pdc_good:
        mov     edx,OFFSET msg_tdone
        mov     bl,A_DLG
        call    tu_box
        jmp     short pdc_out
pdc_saybad:
        ; "Change 2 of 3 could not be made:", what the verb said, and
        ; what was written before it
        call    ln_new
        mov     edx,OFFSET msg_xchange
        call    lb_puts
        mov     eax,[pdc_i]
        inc     eax
        call    lb_num
        mov     edx,OFFSET msg_tcof
        call    lb_puts
        mov     eax,[pdc_n]
        call    lb_num
        mov     edx,OFFSET msg_xnotmade
        call    lb_puts
        mov     ecx,3
        call    ln_captail
        call    ln_new
        mov     edx,OFFSET msg_xnone
        mov     eax,[pdc_i]
        or      eax,eax
        jz      pdc_written
        mov     edx,OFFSET msg_xbefore
        call    lb_puts
        inc     eax
        call    lb_num
        mov     edx,OFFSET msg_xbefore2
pdc_written:
        call    lb_puts
        mov     edx,OFFSET msg_tfailed
        mov     bl,A_DLGERR
        call    tu_box
pdc_out:
        popad
        ret

;--- pd_askquit - quitting with changes waiting.  -> EAX = 0 to commit
;--- them, 1 to throw them away, 2 to stay
pd_askquit:
        push    ebx
        push    edx
        call    dg_reset
        call    ln_new
        mov     eax,[jn_n]
        call    lb_num
        mov     edx,OFFSET msg_qqone
        cmp     eax,1
        je      paq_count
        mov     edx,OFFSET msg_qqmany
paq_count:
        call    lb_puts
        call    ln_new
        mov     edx,OFFSET msg_qquit
        call    lb_puts
        mov     dword ptr [dg_title],OFFSET msg_tquit
        mov     byte ptr [dg_attr],A_DLG
        mov     edx,OFFSET msg_byes
        call    dg_btn
        mov     edx,OFFSET msg_bno
        call    dg_btn
        mov     edx,OFFSET msg_bcancel
        call    dg_btn
        mov     dword ptr [dg_focus],0
        mov     dword ptr [dg_esc],2
        call    dg_run
        pop     edx
        pop     ebx
        ret

;---------------------------------------------------------------------
; tu_locked - CF set when the selected partition is one a change waiting
;             has left unreadable, with the reason built into the box's
;             lines (dg_reset first) for the caller to show.
;---------------------------------------------------------------------
tu_locked:
        pushad
        call    tui_selrow
        mov     eax,[esi+TR_START]
        mov     edx,[tui_disk]
        add     edx,80h
        call    pd_locked
        jnc     tlk_free
        call    dg_reset
        call    ln_new
        call    lb_namec
        mov     edx,OFFSET msg_xlocked
        call    lb_puts
        call    ln_new
        mov     edx,OFFSET msg_xlocked2
        call    lb_puts
        popad
        stc
        ret
tlk_free:
        popad
        clc
        ret

;--- tu_lockbox - tu_locked, and the box shown with EDX -> its title.
;--- CF as tu_locked.
tu_lockbox:
        call    tu_locked
        jnc     tlb_out
        push    ebx
        mov     bl,A_DLGERR
        call    tu_box
        pop     ebx
        stc
tlb_out:
        ret

;--- pd_hints - EDX -> the status line while changes are waiting -----
pd_hints:
        push    eax
        push    ebx
        push    ecx
        push    edi
        mov     edi,OFFSET pd_hintbuf
        mov     eax,[jn_n]
        mov     ebx,1
        call    fmt_n                   ; -> EDX
        call    pdh_cat
        mov     edx,OFFSET msg_shone
        cmp     dword ptr [jn_n],1
        je      pdh_one
        mov     edx,OFFSET msg_shmany
pdh_one:
        call    pdh_cat
        mov     edx,OFFSET msg_shpkeys
        call    pdh_cat
        mov     byte ptr [edi],'$'
        mov     edx,OFFSET pd_hintbuf
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        ret
pdh_cat:                                ; EDX -> a '$' string, onto EDI
        mov     al,[edx]
        cmp     al,'$'
        je      pdh_out
        mov     [edi],al
        inc     edx
        inc     edi
        jmp     short pdh_cat
pdh_out:
        ret

;=====================================================================
; Data for what is waiting.  The overlay and the journal themselves are
; in the BSS (FDISK.ASM).
;=====================================================================
ov_n        dd  0                       ; sectors staged
jn_n        dd  0                       ; changes waiting
ovt_lba     dd  0                       ; ov_patch's range
ovt_cnt     dd  0
ovt_buf     dd  0
pdl_start   dd  0
pdl_drive   dd  0
prs_i       dd  0
pdc_i       dd  0
pdc_n       dd  0
ovt_drv     db  0
ov_full     db  0                       ; a stage ran out of room
pdc_bad     db  0
pd_multi_on db  0
pd_hintbuf  db  TUI_COLS dup(0)

msg_qcommit db  'Write these changes to the disk?$'
msg_qcmore  db  '...and $'
msg_qcmore2 db  ' more.$'
msg_qcdisk  db  'Disk $'
msg_qcdisk2 db  ': $'
msg_tcommit db  'Commit Changes$'
msg_tcwrite db  'Writing change $'
msg_tcof    db  ' of $'
msg_tcdots  db  '...$'
msg_xwritten db 'The changes were written.$'
msg_xrestart db 'Quitting FDISK restarts the system.$'
msg_xchange db  'Change $'
msg_xnotmade db ' could not be made:$'
msg_xnone   db  'Nothing was written.$'
msg_xbefore db  'The changes before change $'
msg_xbefore2 db ' were written; the rest were not.$'
msg_xpending db 'Too many changes are waiting.  Commit them first.$'
msg_xdropped db 'A change waiting could not be staged again.$'
msg_xdropped2 db 'That change and the ones after it were dropped.$'
msg_xlocked db  ' moves or resizes when the changes are committed.$'
msg_xlocked2 db 'Commit or undo the changes waiting first.$'
msg_tnoundo db  'Nothing is waiting to be undone.$'
msg_tnocommit db 'Nothing is waiting to be committed.$'
msg_tundone db  'Undone: $'
msg_qqone   db  ' change is waiting.$'
msg_qqmany  db  ' changes are waiting.$'
msg_qquit   db  'Commit the changes before quitting?$'
msg_tquit   db  'Quit$'
msg_shone   db  ' change waiting.   $'
msg_shmany  db  ' changes waiting.   $'
msg_shpkeys db  'W Commit  U Undo  F1 Help  F10 Menus  Q Quit$'
