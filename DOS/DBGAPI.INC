;=====================================================================
; DBGAPI.INC - a program under a debugger: 21h/F1h AL=29h
;
; DEBUG.EXE is a native program, and the program it debugs is another
; one, one EXEC level below it in a slot of its own.  The kernel keeps
; the two apart for every other pair of programs, so this is the one
; door between them: DEBUG asks, and the kernel loads, runs, stops and
; reads the other program for it.  PMDOS.INC has the interface (DBF_*,
; DRX_*); this is how it works.
;
; HELD IS A STATE 4Bh NEVER HAD.  A child normally exists only while
; it runs: 4Bh builds its world and jumps into it, and 4Ch takes the
; world down and returns through its parent's 4Bh frame.  A held
; program has a world and is not running - its slot, PSP, handles,
; heap, floating-point state and vectors are kept here (dbx_*), and
; the parent runs as if its 4Bh had come back.  DBF_GO is 4Bh's second
; half done again: the parent waits in exec_stk exactly as it waits in
; an EXEC, and the program is entered with the registers DEBUG gives
; it.  dbx_park is 4Ch's second half without the slot going back.
;
; SO A PROGRAM THAT ENDS UNDER DBF_GO NEEDS NOTHING FROM HERE.  Its 4Ch
; unwinds into the frame the parent is waiting in, which is the DBF_GO
; call, and leaves AX = 0 in it - DBF_ENDED.  dbx_onexit only notes
; that nothing is held any more.
;
; WHAT STOPS IT is r3_fault's business first: a fault, an INT 3 (a #GP
; through a gate a program may not use) and a single step (exception
; 1) all arrive there, and r3_fault ends the program.  dbx_trap takes
; them instead while a held program is running at the top of its own
; level.  Inside a handler the kernel called for it (r3_calldep) there
; are kernel frames under the program's, and parking it would leave
; them for the debugger's own DOS calls to write over - so a fault
; there ends it, as it always did.
;
; THE VECTORS ARE SWAPPED, NOT SHARED.  A program's 22h, 23h and 24h
; are live while it runs, and its debugger's must be live while the
; debugger runs - with a held program's INT 23h live, r3_thunk would
; find a handler of another depth and end the program that is running
; (rt_parked), which is the debugger.  So each park lifts the parent's
; three out of the held program's PSP, as 4Ch does, and keeps the
; program's own; each resume puts the program's own back, and writes
; the parent's as they are NOW into the PSP for the program's 4Ch.
;
; Included by DOSMAIN.ASM, after RING3.INC.
;=====================================================================

DBF_MAXIO       EQU     00010000h       ; DBF_READ and DBF_WRITE, at most
DBF_EFLMASK     EQU     00000DD5h       ; the flags DEBUG may set: the six
                                        ; arithmetic ones, TF and DF

;=====================================================================
; fF1_dbg - 21h/F1h AL=29h.  The function is in bits 16-23 of EAX.
;=====================================================================
fF1_dbg:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     i21_unsup
        movzx   eax,byte ptr [ebp+F_EAX+2]
        cmp     eax,DBF_LOAD
        je      fdb_load
        cmp     eax,DBF_GO
        je      fdb_go
        cmp     eax,DBF_READ
        je      fdb_read
        cmp     eax,DBF_WRITE
        je      fdb_write
        cmp     eax,DBF_KILL
        je      fdb_kill
        jmp     i21_unsup

fdb_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
fdb_bad:
        mov     ax,ERR_BADDATA
        jmp     i21_fail
fdb_deep:
        mov     ax,ERR_NOMEM
        jmp     i21_fail

;---------------------------------------------------------------------
; DBF_LOAD - 4Bh's own load, from the top, and then held instead of
; started.  The program's floating-point state needs a slot of its own
; (FPSTATE.INC keeps one per depth below EXEC_MAX), and the shell may
; not do this: its 4Ch reloads the interpreter and sweeps the slots,
; and a debugger there could never be told its program had gone.
;---------------------------------------------------------------------
fdb_load:
        cmp     dword ptr [dbx_state],DBST_NONE
        jne     fdb_deny
        cmp     dword ptr [r3_calldep],0
        jne     fdb_deny
        mov     eax,[exec_depth]
        or      eax,eax
        jz      fdb_deny
        inc     eax
        cmp     eax,EXEC_MAX
        jae     fdb_deep
        mov     eax,[ebp+F_ESI]
        mov     ecx,DRX_LEN
        call    cbuf_ok
        jc      fdb_deny
        mov     [dbx_ans],eax
        call    dbx_tickon
        mov     byte ptr [dbx_loading],1
        jmp     f4B_body

;---------------------------------------------------------------------
; dbx_loaded - 4Bh has loaded the program and its world is the live
;              one, at exec_depth.  EAX = its entry point.  r3_start's
;              registers go into dbx_regs instead of into the CPU.
;---------------------------------------------------------------------
dbx_loaded:
        mov     byte ptr [dbx_loading],0
        push    eax
        mov     edi,OFFSET dbx_regs
        xor     eax,eax
        mov     ecx,DRX_LEN / 4
        rep     stosd
        pop     eax
        mov     dword ptr [dbx_regs+DRX_EIP],eax
        mov     eax,[r3_size]                   ; r3_start's stack: the top of
        sub     eax,4                           ; the slot, with the PSP's
        mov     dword ptr [dbx_regs+DRX_ESP],eax          ; offset on it for a RET
        mov     ebx,[r3_base]
        mov     dword ptr [ebx+eax],SLOT_PSP
        mov     dword ptr [dbx_regs+DRX_EFL],EFL_IF OR 2
        mov     word ptr [dbx_regs+DRX_CS],SEL_PROG_CS
        mov     word ptr [dbx_regs+DRX_DS],SEL_PROG_DS
        mov     word ptr [dbx_regs+DRX_ES],SEL_PROG_DS
        mov     word ptr [dbx_regs+DRX_SS],SEL_PROG_DS
        mov     dword ptr [dbx_regs+DRX_VEC],DRX_NOVEC
        mov     dword ptr [r3_calldep],0        ; r3_start's two
        mov     dword ptr [r3_active],1
        mov     eax,[exec_depth]
        mov     [dbx_depth],eax
        xor     eax,eax                         ; LOAD answers 0
        jmp     dbx_park

;---------------------------------------------------------------------
; DBF_GO - run it with the registers in the caller's record.
;
; THE RECORD IS CHECKED FIRST, because anything the IRETD into it would
; fault on faults in the KERNEL: a CS or SS that is not the program's,
; a data selector it could not load, an EIP past the end of its code
; segment.  Everything else - an ESP in the guard page, an EIP at an
; instruction that faults - is the program's own fault, and arrives
; back here as a stop.
;---------------------------------------------------------------------
fdb_go:
        call    dbx_holder
        jc      fdb_deny
        mov     eax,[ebp+F_ESI]
        mov     ecx,DRX_LEN
        call    cbuf_ok
        jc      fdb_deny
        mov     [dbx_ans],eax
        mov     esi,eax
        mov     edi,OFFSET dbx_new
        mov     ecx,DRX_LEN / 4
        rep     movsd
        call    dbx_check
        jc      fdb_bad
        mov     esi,OFFSET dbx_new              ; the registers and the six
        mov     edi,OFFSET dbx_regs             ; selectors; the rest is ours
        mov     ecx,DRX_VEC / 4
        rep     movsd
        jmp     dbx_resume

; dbx_check - the record in dbx_new can be IRETDed into.  CF if not.
dbx_check:
        cmp     word ptr [dbx_new+DRX_CS],SEL_PROG_CS
        jne     dxc_bad
        cmp     word ptr [dbx_new+DRX_SS],SEL_PROG_DS
        jne     dxc_bad
        mov     ax,word ptr [dbx_new+DRX_DS]
        call    dxc_data
        jc      dxc_bad
        mov     ax,word ptr [dbx_new+DRX_ES]
        call    dxc_data
        jc      dxc_bad
        mov     ax,word ptr [dbx_new+DRX_FS]
        call    dxc_data
        jc      dxc_bad
        mov     ax,word ptr [dbx_new+DRX_GS]
        call    dxc_data
        jc      dxc_bad
        mov     eax,[dbx_depth]
        mov     eax,[r3_slsize+eax*4]
        cmp     dword ptr [dbx_new+DRX_EIP],eax           ; the code segment's limit is
        jae     dxc_bad                         ; the slot's last byte
        clc
        ret
dxc_bad:
        stc
        ret
; AX = a data selector: null, or one of the program's own two
dxc_data:
        or      ax,ax
        jz      dxd_ok
        cmp     ax,SEL_PROG_DS
        je      dxd_ok
        cmp     ax,SEL_PROG_CS                  ; readable code: loadable
        je      dxd_ok
        stc
        ret
dxd_ok:
        clc
        ret

; dbx_holder - CF unless a program is held and the caller holds it,
;              outside any handler of its own.
dbx_holder:
        push    eax
        cmp     dword ptr [dbx_state],DBST_HELD
        jne     dxh_no
        cmp     dword ptr [r3_calldep],0
        jne     dxh_no
        mov     eax,[exec_depth]
        inc     eax
        cmp     eax,[dbx_depth]
        jne     dxh_no
        pop     eax
        clc
        ret
dxh_no:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; DBF_READ and DBF_WRITE - bytes out of and into the held program's
; slot.  Not through the program's descriptors, which are its parent's
; while the parent runs: through the flat address, which r3_slbase has.
;---------------------------------------------------------------------
fdb_read:
        call    dbx_holder
        jc      fdb_deny
        mov     eax,[ebp+F_EDI]
        mov     ecx,[ebp+F_ECX]
        call    cbuf_ok
        jc      fdb_deny
        call    dbx_range
        jc      fdb_bad
        mov     edi,[ebp+F_EDI]
        cld
        rep     movsb
        jmp     i21_ok

fdb_write:
        call    dbx_holder
        jc      fdb_deny
        mov     eax,[ebp+F_ESI]
        mov     ecx,[ebp+F_ECX]
        call    cbuf_ok
        jc      fdb_deny
        call    dbx_range
        jc      fdb_bad
        mov     edi,esi
        mov     esi,[ebp+F_ESI]
        cld
        rep     movsb
        jmp     i21_ok

; dbx_range - EBX and ECX of the call -> ESI = the flat address in the
;             held program's slot, ECX = the count.  CF if any of it is
;             outside the slot or in its guard page, which is not there:
;             the kernel would take the page fault itself.
dbx_range:
        mov     ebx,[ebp+F_EBX]
        mov     ecx,[ebp+F_ECX]
        cmp     ecx,DBF_MAXIO
        ja      dxa_bad
        cmp     ebx,SLOT_PSP
        jb      dxa_bad
        mov     eax,ebx
        add     eax,ecx
        jc      dxa_bad
        mov     esi,[dbx_depth]
        cmp     eax,[r3_slsize+esi*4]
        ja      dxa_bad
        mov     esi,[r3_slbase+esi*4]
        add     esi,ebx
        clc
        ret
dxa_bad:
        stc
        ret

;---------------------------------------------------------------------
; DBF_KILL - the held program ends without running again.
;---------------------------------------------------------------------
fdb_kill:
        call    dbx_holder
        jc      fdb_deny
        call    dbx_release
        jmp     i21_ok

;=====================================================================
; dbx_park - the held program's world is the live one, at exec_depth,
;            with its registers in dbx_regs.  Keep that world here, put
;            the parent's back as 4Ch would, and answer the parent's
;            call - LOAD's or GO's, whichever it is waiting in - with
;            AX = EAX and the registers in its record.
;
; Never returns.  Interrupts are off, as they are in any INT 21h call:
; nothing may run on either world until both are where they belong.
;=====================================================================
dbx_park:
        cli
        push    eax
        mov     dword ptr [dbx_state],DBST_HELD
        mov     eax,[r3_size]                   ; how big its slot is now
        mov     dword ptr [dbx_regs+DRX_LIMIT],eax
        mov     eax,[exec_depth]                ; its floating-point state
        add     eax,FPS_NATIVE
        call    fps_keep

        ; Its three vectors as they stand - its own handlers, or what it
        ; inherited, which the tags tell apart at the next resume.
        mov     al,22h
        xor     ecx,ecx
dxp_vec:
        push    eax
        call    get_vec
        mov     [dbx_vval+ecx*4],eax
        pop     eax
        call    get_vtag
        mov     [dbx_vtag+ecx*4],edx
        inc     al
        inc     ecx
        cmp     ecx,3
        jb      dxp_vec

        ; ...and the parent's out of its PSP, exactly as 4Ch lifts them
        mov     esi,[cur_psp]
        mov     eax,[esi+PSP_TERM]
        mov     [term_vec22],eax
        mov     eax,[esi+PSP_CTRLC]
        mov     [term_vec23],eax
        mov     eax,[esi+PSP_CRITERR]
        mov     [term_vec24],eax
        call    term_vtags

        ; its world
        mov     eax,[cur_psp]
        mov     [dbx_psp],eax
        mov     eax,[cur_own]
        mov     [dbx_own],eax
        mov     eax,[dta_ptr]
        mov     [dbx_dta],eax
        mov     eax,[r3h_lo]
        mov     [dbx_hlo],eax
        mov     eax,[r3h_hi]
        mov     [dbx_hhi],eax

        ; The parent's back, in 4Ch's order.  EBX = its record.
        dec     dword ptr [exec_depth]
        mov     ebx,[exec_depth]
        imul    ebx,XN_LEN
        add     ebx,OFFSET exec_stk
        mov     eax,[exec_depth]
        call    r3_useslot
        mov     eax,[ebx+XN_HLO]
        mov     [r3h_lo],eax
        mov     eax,[ebx+XN_HHI]
        mov     [r3h_hi],eax
        mov     eax,[ebx+XN_PSP]
        mov     [cur_psp],eax
        mov     eax,[ebx+XN_OWN]
        mov     [cur_own],eax
        mov     eax,[ebx+XN_DTA]
        mov     [dta_ptr],eax
        mov     eax,[ebx+XN_INDOS]
        mov     byte ptr ds:[DOS_INDOS],al
        mov     eax,[ebx+XN_ESP0]
        mov     [r3_esp0],eax
        mov     eax,[ebx+XN_LDTR]
        cmp     dword ptr ds:[dpmi_active],0
        jne     dxp_ldt
        xor     eax,eax
dxp_ldt:
        lldt    ax
        mov     eax,[exec_depth]
        add     eax,FPS_NATIVE
        call    fps_back
        call    term_vectors

        ; the registers, to the record the parent named
        mov     esi,OFFSET dbx_regs
        mov     edi,[dbx_ans]
        mov     ecx,DRX_LEN / 4
        cld
        rep     movsd

        ; and out through the frame it is waiting in
        pop     eax
        mov     esp,[ebx+XN_EBP]
        mov     ebp,esp
        mov     ecx,[ebp+F_ESP3]                ; its stack, for a handler of
        mov     [r3_lastesp],ecx                ; its own
        mov     word ptr [ebp+F_EAX],ax
        call    i21_ok
        jmp     i21_return

;=====================================================================
; dbx_resume - the parent's DBF_GO frame is at EBP and the registers
;              to run with are in dbx_regs.  The parent waits as it
;              would in 4Bh; the held program's world goes back in; and
;              into it.  Never returns.
;=====================================================================
dbx_resume:
        cli
        mov     edi,[exec_depth]
        imul    edi,XN_LEN
        add     edi,OFFSET exec_stk
        mov     [edi+XN_EBP],ebp
        mov     eax,[cur_psp]
        mov     [edi+XN_PSP],eax
        mov     eax,[cur_own]
        mov     [edi+XN_OWN],eax
        mov     eax,[dta_ptr]
        mov     [edi+XN_DTA],eax
        movzx   eax,byte ptr ds:[DOS_INDOS]
        mov     [edi+XN_INDOS],eax
        mov     eax,[r3_esp0]                   ; the program's level of the
        mov     [edi+XN_ESP0],eax               ; ring-0 stack starts below
        mov     [r3_esp0],ebp                   ; this call's frame, as in 4Bh
        mov     eax,[r3h_lo]
        mov     [edi+XN_HLO],eax
        mov     eax,[r3h_hi]
        mov     [edi+XN_HHI],eax
        sldt    ax
        movzx   eax,ax
        mov     [edi+XN_LDTR],eax
        mov     eax,[exec_depth]
        add     eax,FPS_NATIVE
        call    fps_keep

        mov     eax,[dbx_depth]
        mov     [exec_depth],eax
        call    r3_useslot

        ; What its PSP gives back at its 4Ch is the parent's three as
        ; they are now, with whose they are (psp_vtags reads r3_depth,
        ; which is the program's from here).
        mov     edi,[dbx_psp]
        mov     al,22h
        call    get_vec
        mov     [edi+PSP_TERM],eax
        mov     al,23h
        call    get_vec
        mov     [edi+PSP_CTRLC],eax
        mov     al,24h
        call    get_vec
        mov     [edi+PSP_CRITERR],eax
        call    psp_vtags

        ; Its own handlers back over them.  One it only inherited is the
        ; parent's of LOAD time, and the parent's of now is the one it
        ; would inherit.
        mov     al,22h
        xor     ecx,ecx
dxr_vec:
        mov     edx,[dbx_vtag+ecx*4]
        cmp     edx,[dbx_depth]
        jne     dxr_next
        mov     edi,[dbx_vval+ecx*4]
        call    set_vec
dxr_next:
        inc     al
        inc     ecx
        cmp     ecx,3
        jb      dxr_vec

        mov     eax,[dbx_psp]
        mov     [cur_psp],eax
        mov     eax,[dbx_own]
        mov     [cur_own],eax
        mov     eax,[dbx_dta]
        mov     [dta_ptr],eax
        mov     eax,[dbx_hlo]
        mov     [r3h_lo],eax
        mov     eax,[dbx_hhi]
        mov     [r3h_hi],eax
        mov     eax,dword ptr [dbx_regs+DRX_ESP]          ; where a handler of its own
        mov     [r3_lastesp],eax                ; would run, until it calls in
        mov     byte ptr ds:[DOS_INDOS],0       ; it runs with DOS idle (4Bh)
        xor     eax,eax
        lldt    ax
        mov     eax,[exec_depth]
        add     eax,FPS_NATIVE
        call    fps_back
        mov     dword ptr [r3_calldep],0
        mov     dword ptr [dbx_state],DBST_RUN
        ; falls into dbx_enter

;---------------------------------------------------------------------
; dbx_enter - r3_enter with every register the record has, and its
;             flags.  IF is set whatever it says and IOPL stays 0, for
;             r3_enter's reasons; NT is cleared in the flags we stand
;             in, for the reason r3_enter tells at length.
;---------------------------------------------------------------------
dbx_enter:
        cli
        pushfd
        and     dword ptr [esp],NOT EFL_NT
        popfd
        mov     eax,[r3_esp0]
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],eax
        movzx   eax,word ptr [dbx_regs+DRX_SS]
        push    eax
        push    dword ptr [dbx_regs+DRX_ESP]
        mov     eax,dword ptr [dbx_regs+DRX_EFL]
        and     eax,DBF_EFLMASK
        or      eax,EFL_IF OR 2
        push    eax
        movzx   eax,word ptr [dbx_regs+DRX_CS]
        push    eax
        push    dword ptr [dbx_regs+DRX_EIP]
        mov     eax,dword ptr [dbx_regs+DRX_EAX]
        mov     ebx,dword ptr [dbx_regs+DRX_EBX]
        mov     ecx,dword ptr [dbx_regs+DRX_ECX]
        mov     edx,dword ptr [dbx_regs+DRX_EDX]
        mov     esi,dword ptr [dbx_regs+DRX_ESI]
        mov     edi,dword ptr [dbx_regs+DRX_EDI]
        mov     ebp,dword ptr [dbx_regs+DRX_EBP]
        mov     fs,word ptr ss:[dbx_regs+DRX_FS]
        mov     gs,word ptr ss:[dbx_regs+DRX_GS]
        mov     es,word ptr ss:[dbx_regs+DRX_ES]
        mov     ds,word ptr ss:[dbx_regs+DRX_DS]
        iretd

;=====================================================================
; dbx_trap - from r3_fault, with its registers: EAX = vector, EDX =
;            error code, EBP -> the frame PMIO built (HK_*), and on the
;            stack above the return address the two dwords pm3_real
;            pushed - FS and GS at ESP+4, DS and ES at ESP+8.
;
; Not ours unless the held program is the one running, at the top of
; its own level; then back to r3_fault with everything as it came.
;=====================================================================
dbx_trap:
        push    eax
        mov     eax,[r3_depth]
        cmp     eax,[dbx_depth]
        pop     eax
        jne     r3f_go
        cmp     dword ptr [r3_calldep],0
        jne     r3f_go

        ; held again before anything else: the tick must not find it
        ; running from here on (dbx_tick)
        mov     dword ptr [dbx_state],DBST_HELD
        mov     dword ptr [dbx_regs+DRX_VEC],eax
        mov     dword ptr [dbx_regs+DRX_ERR],edx
        mov     eax,[ebp+HK_IRET]
        mov     dword ptr [dbx_regs+DRX_EIP],eax
        mov     eax,[ebp+HK_IRET+4]
        mov     word ptr [dbx_regs+DRX_CS],ax
        mov     eax,[ebp+HK_IRET+8]
        mov     dword ptr [dbx_regs+DRX_EFL],eax
        mov     eax,[ebp+HK_IRET+12]
        mov     dword ptr [dbx_regs+DRX_ESP],eax
        mov     eax,[ebp+HK_IRET+16]
        mov     word ptr [dbx_regs+DRX_SS],ax
        mov     eax,[ebp+HK_REGS]               ; PUSHAD's order: EDI first
        mov     dword ptr [dbx_regs+DRX_EDI],eax
        mov     eax,[ebp+HK_REGS+4]
        mov     dword ptr [dbx_regs+DRX_ESI],eax
        mov     eax,[ebp+HK_REGS+8]
        mov     dword ptr [dbx_regs+DRX_EBP],eax
        mov     eax,[ebp+HK_REGS+16]
        mov     dword ptr [dbx_regs+DRX_EBX],eax
        mov     eax,[ebp+HK_REGS+20]
        mov     dword ptr [dbx_regs+DRX_EDX],eax
        mov     eax,[ebp+HK_REGS+24]
        mov     dword ptr [dbx_regs+DRX_ECX],eax
        mov     eax,[ebp+HK_REGS+28]
        mov     dword ptr [dbx_regs+DRX_EAX],eax
        mov     eax,[esp+8]
        mov     word ptr [dbx_regs+DRX_DS],ax
        shr     eax,16
        mov     word ptr [dbx_regs+DRX_ES],ax
        mov     eax,[esp+4]
        mov     word ptr [dbx_regs+DRX_FS],ax
        shr     eax,16
        mov     word ptr [dbx_regs+DRX_GS],ax

        ; the address a page fault reached for, as the program names it
        xor     eax,eax
        cmp     dword ptr [dbx_regs+DRX_VEC],14
        jne     dxt_nopf
        mov     eax,cr2
        sub     eax,[r3_base]
dxt_nopf:
        mov     dword ptr [dbx_regs+DRX_ADDR],eax

        ; A single step's DR6, and DR6 cleared after it: its bits are
        ; sticky, and the null guard reads them (NULLTRAP.INC).
        xor     eax,eax
        cmp     dword ptr [dbx_regs+DRX_VEC],1
        jne     dxt_nodb
        mov     eax,dr6
        xor     ecx,ecx
        mov     dr6,ecx
dxt_nodb:
        mov     dword ptr [dbx_regs+DRX_DR6],eax

        ; The frame is copied, so the stack goes back to the top of the
        ; program's level, as r3_fault puts it - and dbx_park moves on
        ; to the parent's from there.
        mov     esp,[r3_esp0]
        mov     eax,DBF_STOPPED
        jmp     dbx_park

;=====================================================================
; dbx_release - the held program ends where it is: what 4Ch does for
;               a program, but for one that is not running and never
;               will again.  Its parent is the one running.  Everything
;               preserved.
;
; Its 22h, 23h and 24h need nothing: the parent's are the live ones
; while it is held.  What it hooked besides those - 1Bh, 1Ch, 28h, or
; any vector it only told 25h about - would be a handler at its depth
; with nobody's code behind it, and the next program at that depth
; would be sent into its own slot at that offset; those are forgotten.
;=====================================================================
dbx_release:
        pushad
        mov     esi,OFFSET sft_tbl + 3 * SFT_LEN
        mov     ecx,MAX_HANDLES - 3
        mov     edx,[dbx_own]
dxl_sweep:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      dxl_next
        cmp     [esi+SFT_OWNER],edx
        jne     dxl_next
        push    edx
        call    sft_close
        pop     edx
dxl_next:
        add     esi,SFT_LEN
        loop    dxl_sweep
        call    share_relown                    ; EDX = the owner
        call    find_freeown
        mov     eax,[dbx_own]
        call    arena_freeown
        call    xms_relown
        call    dos_commit

        mov     eax,[dbx_depth]                 ; its mouse handlers (4Ch's
        inc     eax                             ; numbering: depth + 1)
        call    dword ptr ds:[IOS_MSEXIT]
        mov     eax,[dbx_depth]                 ; its video mode
        call    vbe_exit

        xor     ebx,ebx
dxl_hook:
        mov     eax,[r3vdep+ebx*4]
        cmp     eax,[dbx_depth]
        jne     dxl_nohook
        cmp     dword ptr [r3vec+ebx*4],0
        je      dxl_nohook
        mov     eax,ebx
        call    r3_unhook
dxl_nohook:
        inc     ebx
        cmp     ebx,256
        jb      dxl_hook

        mov     eax,[dbx_depth]
        call    slot_free
        mov     dword ptr [dbx_state],DBST_NONE
        popad
        ret

;---------------------------------------------------------------------
; dbx_onexit - from 4Ch, for the program ending at exec_depth.  If it
;              is the held program, running, nothing is held any more;
;              if it is the program holding one, that one ends first.
;              Everything preserved.
;---------------------------------------------------------------------
dbx_onexit:
        cmp     dword ptr [dbx_state],DBST_NONE
        je      dxo_out
        push    eax
        mov     eax,[exec_depth]
        cmp     eax,[dbx_depth]
        je      dxo_self
        inc     eax
        cmp     eax,[dbx_depth]
        jne     dxo_pop
        cmp     dword ptr [dbx_state],DBST_HELD
        jne     dxo_pop
        call    dbx_release
        jmp     short dxo_pop
dxo_self:
        mov     dword ptr [dbx_state],DBST_NONE
dxo_pop:
        pop     eax
dxo_out:
        ret

;---------------------------------------------------------------------
; dbx_mayexec - for 4Bh: CF if the caller holds a program.  Its slot is
;               the next depth's, and an EXEC would build another there.
;---------------------------------------------------------------------
dbx_mayexec:
        cmp     dword ptr [dbx_state],DBST_HELD
        jne     dxm_ok
        push    eax
        mov     eax,[exec_depth]
        inc     eax
        cmp     eax,[dbx_depth]
        pop     eax
        jne     dxm_ok
        stc
        ret
dxm_ok:
        clc
        ret

;---------------------------------------------------------------------
; dbx_tickon - DBF_LOAD: hook the clock once, for Ctrl-Break.  A full
;              table only means the key cannot stop a program.
;
; dbx_tick - every tick, interrupts off.  Ctrl-Break while the held
;            program runs at the top of its own level: TF goes into the
;            frame it will next go back to ring 3 through - the one the
;            interrupt that is running now pushed, or the one its open
;            DOS call came in on - and it stops after one instruction,
;            as a single step.  That frame is the five dwords under
;            r3_esp0, whichever it is.  The Break key's mark is taken,
;            and the 0000h the keyboard put behind it, so the program
;            is not handed a ^C as well.  A program in a DOS call that
;            looks for a break first gets the ^C instead, as it would
;            have without a debugger.
;---------------------------------------------------------------------
dbx_tickon:
        cmp     byte ptr [dbx_ticked],0
        jne     dxn_out
        pushad
        mov     edi,OFFSET dbx_tick
        call    dword ptr ds:[IOS_TICKHOOK]
        jc      dxn_full
        mov     byte ptr [dbx_ticked],1
dxn_full:
        popad
dxn_out:
        ret

dbx_tick:
        cmp     dword ptr ds:[dbx_state],DBST_RUN
        jne     dxk_out
        cmp     byte ptr ds:[DOSDATA + GNLS_BRKFLAG],0
        je      dxk_out
        cmp     dword ptr ds:[r3_calldep],0
        jne     dxk_out
        mov     eax,ds:[r3_depth]
        cmp     eax,ds:[dbx_depth]
        jne     dxk_out
        mov     ebx,ds:[r3_esp0]
        cmp     word ptr [ebx-16],SEL_PROG_CS
        jne     dxk_out
        or      dword ptr [ebx-12],EFL_TF
        mov     byte ptr ds:[DOSDATA + GNLS_BRKFLAG],0
        mov     ax,word ptr ds:[BDA_KBTAIL]
        mov     word ptr ds:[BDA_KBHEAD],ax
dxk_out:
        ret

;=====================================================================
; Data
;=====================================================================
dbx_state       dd  DBST_NONE
dbx_depth       dd  0                   ; the held program's EXEC depth
dbx_ans         dd  0                   ; the caller's record, flat
dbx_psp         dd  0                   ; the held program's world:
dbx_own         dd  0                   ;   cur_psp, cur_own, dta_ptr and
dbx_dta         dd  0                   ;   its heap's bounds
dbx_hlo         dd  0
dbx_hhi         dd  0
dbx_vval        dd  3 dup(0)            ; its 22h-24h, and whose they were
dbx_vtag        dd  3 dup(-1)
dbx_loading     db  0                   ; 4Bh is loading for DBF_LOAD
dbx_ticked      db  0                   ; dbx_tick is on the clock
                db  0, 0
dbx_regs        db  DRX_LEN dup(0)      ; the held program's registers
dbx_new         db  DRX_LEN dup(0)      ; ...and a record being checked
