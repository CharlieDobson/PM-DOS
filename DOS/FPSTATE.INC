;=====================================================================
; FPSTATE.INC - a program's floating-point state, a level at a time
;
; THERE IS NO TASK SWITCH IN PM-DOS, so there is one place and one only
; where one program's coprocessor state can become another's: EXEC.  A
; parent is waiting in its 4Bh with whatever it had in the registers,
; and the child runs in front of it in the same ones.  A child that
; ends with values still on the stack, with the rounding mode changed,
; or with MMX registers it never emptied - which marks every register
; in use, so that the parent's next load is a stack overflow - hands
; all of it back, and DOS has always let it.
;
; So the parent's state is put away when the child starts and put back
; when it ends, and the child begins with a fresh one.  PMIO does the
; keeping (IOS_FPU) because it knows how this processor keeps a state;
; what is here is where, and when.
;
; WHERE IS A SLOT A LEVEL, and the levels are the ones the kernel
; already counts: a native program's is its depth in exec_stk, a DOS
; program's its depth in vdm_stack, and the two ranges follow each
; other.  That is what makes every way out safe without naming any of
; them - a ^C, a fault, an Abort and a program that simply ends all
; leave through fn4C or vdm_pop, which is where the level is given
; back.
;
; Out of the arena, for find_init's reason: sixteen states are 8K, the
; image has a ceiling, and a machine with no coprocessor needs none.
;
; Included by DOSMAIN.ASM, after VDM.INC.  The slot numbers (FPS_*)
; are with the nesting record in INT21.INC.
;=====================================================================

;---------------------------------------------------------------------
; fps_boot - once, from dos_entry, after arena_init.
;---------------------------------------------------------------------
fps_boot:
        pushad
        mov     al,FPU_INFO
        call    dword ptr ds:[IOS_FPU]
        mov     [fps_flags],eax
        test    al,FPUF_X87
        jz      fpb_out
        push    dword ptr [cur_own]
        mov     dword ptr [cur_own],0
        mov     ebx,FPS_SLOTS * FPU_AREA + 16
        call    arena_alloc
        pop     dword ptr [cur_own]
        jc      fpb_out
        add     eax,15                  ; FXSAVE faults on anything else
        and     eax,NOT 15
        mov     [fps_mem],eax
fpb_out:
        popad
        ret

;---------------------------------------------------------------------
; fps_keep - EAX = a slot.  The state as it stands is put there, and
;            what is left in the registers is a fresh one.
; fps_back - EAX = a slot.  What fps_keep put there is the state again;
;            if nothing was, a fresh one is.
;
; Everything preserved, by both.
;---------------------------------------------------------------------
fps_keep:
        pushad
        cmp     dword ptr [fps_mem],0
        je      fpk_out
        cmp     eax,FPS_SLOTS
        jae     fpk_fresh
        mov     byte ptr [fps_held + eax],1
        mov     edi,eax
        imul    edi,FPU_AREA
        add     edi,[fps_mem]
        mov     al,FPU_KEEP
        call    dword ptr ds:[IOS_FPU]
        jmp     short fpk_out
fpk_fresh:
        mov     al,FPU_FRESH
        call    dword ptr ds:[IOS_FPU]
fpk_out:
        popad
        ret

fps_back:
        pushad
        cmp     dword ptr [fps_mem],0
        je      fpr_out
        cmp     eax,FPS_SLOTS
        jae     fpr_fresh
        cmp     byte ptr [fps_held + eax],0
        je      fpr_fresh
        mov     byte ptr [fps_held + eax],0
        mov     esi,eax
        imul    esi,FPU_AREA
        add     esi,[fps_mem]
        mov     al,FPU_BACK
        call    dword ptr ds:[IOS_FPU]
        jmp     short fpr_out
fpr_fresh:
        mov     al,FPU_FRESH
        call    dword ptr ds:[IOS_FPU]
fpr_out:
        popad
        ret

;---------------------------------------------------------------------
; fps_reset - the interpreter is starting: nothing is waiting under it,
;             so nothing is held, and it begins as any program does.
;             Everything preserved.
;---------------------------------------------------------------------
fps_reset:
        pushad
        mov     edi,OFFSET fps_held
        mov     ecx,FPS_SLOTS
        xor     al,al
        cld
        rep     stosb
        mov     al,FPU_FRESH
        call    dword ptr ds:[IOS_FPU]
        popad
        ret

fps_flags       dd      0               ; FPUF_*, as PMIO found them
fps_mem         dd      0               ; the slots, 0 = there are none
fps_held        db      FPS_SLOTS dup(0)        ; a state is in it
