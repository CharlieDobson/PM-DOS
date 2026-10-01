;=====================================================================
; FPUTEST.ASM - FPUTEST.COM, the coprocessor, MMX and SSE from inside a
;               DOS box.
;
; A DOS BOX CANNOT DO ANY OF THIS FOR ITSELF.  Under plain DOS a
; program is at ring 0: it sets CR4 if it wants SSE, the BIOS has left
; interrupt line 13 open, and the coprocessor's error arrives on INT
; 75h because the board is wired that way.  In a box it is at ring 3
; of somebody else's machine, and every one of those is the host's to
; have done - which PM-DOS had not (IO\FPU.INC).
;
;    1  a coprocessor answers
;    2  ...and the kernel says the same (21h/F1h AL=13h)
;    3  it can add
;    4  MMX, if the processor has it
;    5  SSE runs, if the processor has it - the one that needs CR4
;    6  an error the program unmasked reaches its INT 75h handler
;    7  ...once, with divide-by-zero in the status word, and the
;       program carries on from where it was
;    8  a child starts with a fresh state
;    9  the control word and the stack are the parent's own again
;       after a child that left its own behind
;   10  ...and so is MXCSR
;
; 8 to 10 are this program running itself, as ALLOCTST does: the child
; (FPUTEST /C) says what it was given, then fills the stack, changes
; the control word, leaves the MMX registers in use and exits.
;
; RUN UNDER PLAIN DOS, 8 AND 9 FAIL, and that is the control: DOS gives
; a child its parent's state and gives the parent back the child's.
;
; FPUTEST /S is the one that cannot be survived: the handler on INT 75h
; clears nothing, so the error is reported again the moment it returns.
; The box should end after two seconds, with a reason.
;
; 6 AND 7 NEED A COPROCESSOR THAT REPORTS ITS ERRORS, and an emulator's
; may not: DOSBox's hands back infinity whatever was unmasked.  The
; status word says which kind this is before the kernel comes into it,
; and on one that reports nothing the two are passed over, not failed.
; TOOLS\FPU86.PS1 runs them where they mean something.
;
; Assembled with -fpi87: without it wasm writes every coprocessor
; instruction as a fixup for an emulator to patch.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
        .387
_FP     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_FP, DS:_FP, ES:_FP, SS:_FP
        ORG     0

STACK_BYTES     EQU     512

KF_X87          EQU     01h             ; FPUF_*, as 21h/F1h AL=13h answers
KF_FXSR         EQU     02h
KF_SSE          EQU     04h
KF_SSE2         EQU     08h
KF_MMX          EQU     10h
KF_NE           EQU     20h
KF_IRQ13        EQU     40h

CF_MMX          EQU     0080h           ; CPUID's EDX, the high word of it
CF_SSE          EQU     0200h

CW_RESET        EQU     037Fh           ; what FNINIT leaves
CW_UNMASK       EQU     037Bh           ; ...with divide-by-zero unmasked
CW_MINE         EQU     0F7Fh           ; the parent's: round towards zero
CW_CHILD        EQU     027Fh           ; the child's: 53 bits, to nearest
MX_RESET        EQU     1F80h
MX_MINE         EQU     7F80h           ; round towards zero
MX_CHILD        EQU     9F80h           ; flush to zero

start:
        mov     si,81h
        mov     cl,ds:[80h]
        xor     ch,ch
        or      cx,cx
        jz      be_parent
tl_scan:
        lodsb
        cmp     al,'/'
        jne     tl_next
        mov     al,[si]
        or      al,20h
        cmp     al,'c'
        je      be_child
        cmp     al,'s'
        je      be_stuck
tl_next:
        loop    tl_scan
        jmp     be_parent

;=====================================================================
; THE CHILD.  What was it given, and then: leave a mess.
;=====================================================================
be_child:
        ; LOOKED AT BEFORE ANYTHING IS INITIALISED.  Finding out
        ; whether there is a coprocessor begins with FNINIT, which is
        ; the end of whatever this program was started with.
        call    probe_cpu
        mov     word ptr [cw_was],5A5Ah
        fnstcw  word ptr [cw_was]
        fnstenv [envbuf]
        test    byte ptr [kflags],KF_SSE
        jz      ch_look
        db      0Fh,0AEh,1Eh            ; STMXCSR [mx_got]
        dw      OFFSET mx_got
ch_look:
        call    probe_87
        xor     bl,bl                   ; the exit code: 0 is "fresh"
        cmp     byte ptr [have87],0
        je      ch_exit
        cmp     word ptr [cw_was],CW_RESET
        jne     ch_stale
        test    word ptr [envbuf+2],3800h       ; the stack is at the top
        jnz     ch_stale
        cmp     word ptr [envbuf+4],0FFFFh      ; ...and every register empty
        jne     ch_stale
        test    byte ptr [kflags],KF_SSE
        jz      ch_mess
        cmp     word ptr [mx_got],MX_RESET
        je      ch_mess
ch_stale:
        mov     bl,2

ch_mess:
        fninit
        mov     word ptr [cw_got],CW_CHILD
        fldcw   word ptr [cw_got]
        mov     cx,8
ch_fill:
        fldz
        loop    ch_fill
        test    byte ptr [kflags],KF_SSE
        jz      ch_mmx
        mov     word ptr [mx_got],MX_CHILD
        mov     word ptr [mx_got+2],0
        db      0Fh,0AEh,16h            ; LDMXCSR [mx_got]
        dw      OFFSET mx_got
ch_mmx:
        test    word ptr [cpufeat+2],CF_MMX
        jz      ch_exit
        mov     eax,12345678h
        db      0Fh,6Eh,0C0h            ; MOVD MM0,EAX - and no EMMS
ch_exit:
        mov     al,bl
        mov     ah,4Ch
        int     21h

;=====================================================================
; /S - the handler that clears nothing.
;=====================================================================
be_stuck:
        call    probe_cpu
        mov     dx,OFFSET msg_stuck
        call    puts
        push    ds
        mov     dx,OFFSET bare_iret
        mov     ax,2575h
        int     21h
        pop     ds
        fninit
        mov     word ptr [cw_got],CW_UNMASK
        fldcw   word ptr [cw_got]
        fld1
        fldz
        fdivp   st(1),st
        fnstsw  word ptr [sw_pre]
        fwait                           ; ...and round, and round
        fninit
        call    unhook6
        ; AN INTERRUPT IS DELIVERED ONCE.  Where the error comes in on
        ; line 13 the handler was called, returned, and the FWAIT went
        ; ahead - the kernel had let go of the line for it, as the
        ; BIOS's handler does - so there is nothing to be stuck on, and
        ; being here is the right answer.
        mov     dx,OFFSET msg_byline
        test    byte ptr [kflags],KF_IRQ13
        jnz     st_say
        mov     dx,OFFSET msg_nostuck
        test    word ptr [sw_pre],0080h
        jnz     st_say
        mov     dx,OFFSET msg_mute      ; see t6_none
st_say:
        call    puts
        mov     ax,4C01h
        int     21h

bare_iret:
        iret

;=====================================================================
; THE PARENT.
;=====================================================================
be_parent:
        mov     dx,OFFSET msg_hi
        call    puts

        mov     bx,OFFSET prog_end      ; room for a child - see ALLOCTST
        add     bx,STACK_BYTES
        add     bx,15
        shr     bx,4
        add     bx,10h
        mov     ah,4Ah
        int     21h
        jc      no_shrink
        cli
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti

        call    probe

        mov     dx,OFFSET msg_kern
        call    puts
        cmp     byte ptr [kanswer],0
        jne     pa_kshow
        mov     dx,OFFSET msg_kdumb
        call    puts
        jmp     short pa_cpu
pa_kshow:
        mov     al,[kflags]
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts
pa_cpu:
        mov     dx,OFFSET msg_cpu
        call    puts
        cmp     byte ptr [havecpuid],0
        jne     pa_cshow
        mov     dx,OFFSET msg_nocpuid
        call    puts
        jmp     short t1
pa_cshow:
        mov     ax,word ptr [cpufeat+2]
        call    put4
        mov     ax,word ptr [cpufeat]
        call    put4
        mov     dx,OFFSET msg_crlf
        call    puts
        mov     dx,OFFSET msg_crlf
        call    puts

;--- 1: a coprocessor answers ------------------------------------------
t1:
        mov     dx,OFFSET t_there
        call    puts
        cmp     byte ptr [have87],0
        jne     t1_ok
        mov     dx,OFFSET msg_none
        call    puts
        jmp     t2
t1_ok:
        call    ok

;--- 2: and the kernel agrees ------------------------------------------
t2:
        mov     dx,OFFSET t_agree
        call    puts
        cmp     byte ptr [kanswer],0
        je      t2_bad
        mov     al,[kflags]
        and     al,KF_X87
        cmp     al,[have87]
        jne     t2_bad
        call    ok
        jmp     short t3
t2_bad:
        call    bad

t3:
        cmp     byte ptr [have87],0
        je      done

;--- 3: it can add -----------------------------------------------------
        mov     dx,OFFSET t_add
        call    puts
        fninit
        mov     word ptr [i1],2
        mov     word ptr [i2],3
        fild    word ptr [i1]
        fiadd   word ptr [i2]
        fistp   word ptr [i3]
        cmp     word ptr [i3],5
        jne     t3_bad
        call    ok
        jmp     short t4
t3_bad:
        call    bad

;--- 4: MMX ------------------------------------------------------------
t4:
        mov     dx,OFFSET t_mmx
        call    puts
        test    word ptr [cpufeat+2],CF_MMX
        jnz     t4_go
        mov     dx,OFFSET msg_notcpu
        call    puts
        jmp     short t5
t4_go:
        mov     byte ptr [ud_hit],0
        mov     word ptr [ud_len],3
        mov     eax,00010203h
        db      0Fh,6Eh,0C0h            ; MOVD  MM0,EAX
        db      0Fh,0FEh,0C0h           ; PADDD MM0,MM0
        db      0Fh,7Eh,0C0h            ; MOVD  EAX,MM0
        mov     word ptr [ud_len],2
        db      0Fh,77h                 ; EMMS
        cmp     byte ptr [ud_hit],0
        jne     t4_bad
        cmp     eax,00020406h
        jne     t4_bad
        call    ok
        jmp     short t5
t4_bad:
        call    bad

;--- 5: SSE ------------------------------------------------------------
; The processor having it is not enough, and that is the test: with
; OSFXSR clear every one of these is an invalid opcode.
t5:
        mov     dx,OFFSET t_sse
        call    puts
        test    word ptr [cpufeat+2],CF_SSE
        jnz     t5_go
        mov     dx,OFFSET msg_notcpu
        call    puts
        jmp     short t6
t5_go:
        mov     byte ptr [ud_hit],0
        mov     word ptr [ud_len],5
        db      0Fh,10h,06h             ; MOVUPS XMM0,[v_in]
        dw      OFFSET v_in
        mov     word ptr [ud_len],3
        db      0Fh,58h,0C0h            ; ADDPS  XMM0,XMM0
        mov     word ptr [ud_len],5
        db      0Fh,11h,06h             ; MOVUPS [v_out],XMM0
        dw      OFFSET v_out
        cmp     byte ptr [ud_hit],0
        jne     t5_ud
        cmp     dword ptr [v_out],40000000h     ; 2.0
        jne     t5_bad
        cmp     dword ptr [v_out+12],41000000h  ; 8.0
        jne     t5_bad
        test    byte ptr [kflags],KF_SSE
        jz      t5_bad                  ; it ran, and the kernel says not
        call    ok
        jmp     short t6
t5_ud:
        mov     dx,OFFSET msg_ud
        call    puts
t5_bad:
        call    bad

;--- 6 and 7: an unmasked error ---------------------------------------
t6:
        mov     ax,3575h
        int     21h
        mov     [old75_off],bx
        mov     [old75_seg],es
        push    cs
        pop     es
        mov     dx,OFFSET int75_hook
        mov     ax,2575h
        int     21h

        mov     dx,OFFSET t_err
        call    puts
        fninit
        mov     word ptr [cw_got],CW_UNMASK
        fldcw   word ptr [cw_got]
        fld1
        fldz
fp_from:
        fdivp   st(1),st                ; one over nought
        fnstsw  word ptr [sw_pre]       ; (which does not wait, or report)
        fwait                           ; ...which is reported here
        nop
        nop
fp_to:
        fninit
        cmp     byte ptr [fp_hits],0
        je      t6_none
        call    ok
        jmp     short t7

        ; NOTHING ARRIVED.  Whose fault that is, the status word taken
        ; before the FWAIT says: a coprocessor with an error to report
        ; has its summary bit set, and one that has not set it was
        ; never going to report anything.  DOSBox's is such a one - it
        ; hands back infinity and carries on, under any DOS.
t6_none:
        test    word ptr [sw_pre],0080h
        jnz     t6_bad
        mov     dx,OFFSET msg_mute
        call    puts
        jmp     t8
t6_bad:
        call    bad

t7:
        mov     dx,OFFSET t_once
        call    puts
        mov     al,[fp_hits]
        call    put2
        mov     dl,' '
        call    putc
        mov     ax,[fp_sw]
        call    put4
        mov     dl,' '
        call    putc
        mov     ax,[fp_ip]
        call    put4
        cmp     byte ptr [fp_hits],1
        jne     t7_bad
        test    word ptr [fp_sw],0004h  ; ZE
        jz      t7_bad
        cmp     word ptr [fp_ip],OFFSET fp_from
        jb      t7_bad
        cmp     word ptr [fp_ip],OFFSET fp_to
        ja      t7_bad
        call    ok
        jmp     short t8
t7_bad:
        call    bad

;--- 8 to 10: a child ---------------------------------------------------
t8:
        push    ds
        mov     dx,[old75_off]
        mov     ds,[old75_seg]
        mov     ax,2575h
        int     21h
        pop     ds

        call    whoami

        fninit
        mov     word ptr [cw_got],CW_MINE
        fldcw   word ptr [cw_got]
        fldpi
        fld1
        test    byte ptr [kflags],KF_SSE
        jz      t8_exec
        mov     word ptr [mx_got],MX_MINE
        mov     word ptr [mx_got+2],0
        db      0Fh,0AEh,16h            ; LDMXCSR [mx_got]
        dw      OFFSET mx_got

t8_exec:
        mov     word ptr [pb_env],0
        mov     word ptr [pb_tail],OFFSET tail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs
        mov     cs:[exec_sp],sp
        mov     dx,OFFSET childpath
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        cli
        mov     bx,cs
        mov     ss,bx
        mov     sp,cs:[exec_sp]
        sti
        push    cs
        pop     ds
        push    cs
        pop     es
        jc      t8_noexec

        ; What is in the registers now is the thing being tested, so it
        ; is taken out of them before anything is printed.
        fnstcw  word ptr [cw_got]
        fnstsw  word ptr [sw_got]
        mov     word ptr [i1],0
        mov     word ptr [i2],0
        fnclex
        fistp   word ptr [i1]
        fistp   word ptr [i2]
        fnstsw  word ptr [sw_after]
        test    byte ptr [kflags],KF_SSE
        jz      t8_say
        db      0Fh,0AEh,1Eh            ; STMXCSR [mx_got]
        dw      OFFSET mx_got
t8_say:
        fninit

        mov     dx,OFFSET t_fresh
        call    puts
        mov     ah,4Dh
        int     21h
        or      al,al
        jnz     t8_bad
        call    ok
        jmp     short t9
t8_bad:
        call    bad
        jmp     short t9
t8_noexec:
        fninit
        mov     dx,OFFSET t_fresh
        call    puts
        mov     dx,OFFSET msg_noexec
        call    puts
        mov     dx,OFFSET childpath
        call    putz
        call    bad
        jmp     done

t9:
        mov     dx,OFFSET t_mine
        call    puts
        mov     ax,[cw_got]
        call    put4
        mov     dl,' '
        call    putc
        mov     ax,[sw_got]
        call    put4
        cmp     word ptr [cw_got],CW_MINE
        jne     t9_bad
        mov     ax,[sw_got]             ; two on the stack: the top is 6
        and     ax,3800h
        cmp     ax,3000h
        jne     t9_bad
        cmp     word ptr [i1],1
        jne     t9_bad
        cmp     word ptr [i2],3         ; pi, towards zero
        jne     t9_bad
        test    word ptr [sw_after],0041h       ; ...and neither was a
        jnz     t9_bad                          ; stack fault
        call    ok
        jmp     short t10
t9_bad:
        call    bad

t10:
        mov     dx,OFFSET t_mxcsr
        call    puts
        test    byte ptr [kflags],KF_SSE
        jnz     t10_go
        mov     dx,OFFSET msg_nosse
        call    puts
        jmp     short done
t10_go:
        mov     ax,word ptr [mx_got]
        call    put4
        cmp     word ptr [mx_got],MX_MINE
        jne     t10_bad
        call    ok
        jmp     short done
t10_bad:
        call    bad

done:
        call    unhook6
        mov     dx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     failed
        mov     dx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
failed:
        mov     dx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

no_shrink:
        mov     dx,OFFSET msg_noshrink
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; probe - what is here: the coprocessor, CPUID's features and what the
;         kernel says.  INT 06h is hooked from here on, because finding
;         out whether a processor has an instruction is running it.
;=====================================================================
probe:
        call    probe_cpu
        ; fall through

probe_87:
        mov     word ptr [cw_got],5A5Ah
        fninit
        fnstsw  word ptr [cw_got]
        cmp     word ptr [cw_got],0
        jne     p87_out
        fnstcw  word ptr [cw_got]
        mov     ax,[cw_got]
        and     ax,103Fh
        cmp     ax,003Fh
        jne     p87_out
        mov     byte ptr [have87],1
p87_out:
        ret

probe_cpu:
        mov     ax,3506h
        int     21h
        mov     [old6_off],bx
        mov     [old6_seg],es
        push    cs
        pop     es
        mov     dx,OFFSET int6_hook
        mov     ax,2506h
        int     21h
        mov     byte ptr [hooked6],1

        mov     byte ptr [ud_hit],0
        mov     word ptr [ud_len],2
        xor     eax,eax
        db      0Fh,0A2h                ; CPUID
        cmp     byte ptr [ud_hit],0
        jne     pr_kern
        or      eax,eax
        jz      pr_kern
        mov     eax,1
        db      0Fh,0A2h
        mov     [cpufeat],edx
        mov     byte ptr [havecpuid],1

pr_kern:
        mov     ax,0F113h
        int     21h
        jc      pr_out
        mov     [kflags],al
        mov     byte ptr [kanswer],1
pr_out:
        ret

;---------------------------------------------------------------------
; whoami - this program's own path, from the end of the environment,
;          into childpath.  Left as it is if there is none.
;---------------------------------------------------------------------
whoami:
        push    es
        push    si
        push    di
        mov     ax,ds:[2Ch]
        or      ax,ax
        jz      wh_out
        mov     es,ax
        xor     di,di
wh_scan:
        cmp     word ptr es:[di],0
        je      wh_end
        inc     di
        cmp     di,7F00h
        jb      wh_scan
        jmp     short wh_out
wh_end:
        add     di,2
        cmp     word ptr es:[di],1
        jne     wh_out
        add     di,2
        mov     si,OFFSET childpath
        mov     cx,79
wh_copy:
        mov     al,es:[di]
        mov     [si],al
        inc     di
        inc     si
        or      al,al
        jz      wh_out
        loop    wh_copy
        mov     byte ptr [si],0
wh_out:
        pop     di
        pop     si
        pop     es
        ret

;=====================================================================
; the handlers
;=====================================================================
; INT 06h - step over whatever it was; ud_len says how long.
int6_hook:
        push    bp
        mov     bp,sp
        push    ax
        mov     byte ptr cs:[ud_hit],1
        mov     ax,cs:[ud_len]
        add     [bp+2],ax
        pop     ax
        pop     bp
        iret

; INT 75h - what a run-time library's does: look, clear, and tell both
; interrupt controllers it is finished with.
int75_hook:
        push    ax
        push    bp
        mov     bp,sp
        inc     byte ptr cs:[fp_hits]
        mov     ax,[bp+4]
        mov     cs:[fp_ip],ax
        fnstsw  word ptr cs:[fp_sw]
        fnclex
        xor     al,al
        out     0F0h,al
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     bp
        pop     ax
        iret

unhook6:
        cmp     byte ptr [hooked6],0
        je      uh_out
        push    ds
        mov     dx,[old6_off]
        mov     ds,[old6_seg]
        mov     ax,2506h
        int     21h
        pop     ds
uh_out:
        ret

;=====================================================================
; helpers
;=====================================================================
ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
        pop     ax
        ret
putz:                                   ; DX -> ASCIIZ
        push    ax
        push    dx
        push    si
        mov     si,dx
pz_one:
        lodsb
        or      al,al
        jz      pz_out
        mov     dl,al
        call    putc
        jmp     short pz_one
pz_out:
        pop     si
        pop     dx
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
; data
;=====================================================================
cpufeat     dd  0
v_in        dd  3F800000h, 40000000h, 40400000h, 40800000h  ; 1 2 3 4
v_out       dd  4 dup(0)
mx_got      dd  0
old6_off    dw  0
old6_seg    dw  0
old75_off   dw  0
old75_seg   dw  0
ud_len      dw  2
cw_got      dw  0
cw_was      dw  0
sw_got      dw  0
sw_pre      dw  0
sw_after    dw  0
fp_sw       dw  0
fp_ip       dw  0
i1          dw  0
i2          dw  0
i3          dw  0
exec_sp     dw  0
pb_env      dw  0
pb_tail     dd  0
pb_fcb1     dd  0
pb_fcb2     dd  0
kflags      db  0
kanswer     db  0
have87      db  0
havecpuid   db  0
hooked6     db  0
ud_hit      db  0
fp_hits     db  0
fails       db  0
tail        db  3,' /C',0Dh
envbuf      db  32 dup(0)
childpath   db  'C:\TESTS\FPUTEST.COM',0
            db  60 dup(0)

msg_hi      db  13,10,'FPUTEST - the coprocessor, MMX and SSE in a DOS box',13,10,13,10,'$'
msg_kern    db  '  the kernel says (21h/F1h AL=13h)  $'
msg_kdumb   db  'nothing: it has no such call',13,10,'$'
msg_cpu     db  '  CPUID says                        $'
msg_nocpuid db  'nothing: there is no CPUID',13,10,13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_none    db  ' there is none',13,10,'$'
msg_notcpu  db  ' not in this processor',13,10,'$'
msg_nosse   db  ' no SSE here',13,10,'$'
msg_ud      db  ' invalid opcode$'
msg_noexec  db  ' could not run $'
msg_noshrink db 13,10,'FPUTEST: 4Ah would not shrink us; no room to EXEC',13,10,'$'
msg_pass    db  'FPUTEST: all checks passed',13,10,'$'
msg_fail    db  'FPUTEST: FAILURES - see above',13,10,'$'
msg_stuck   db  'INT 75h now points at a bare IRET, and here comes one over',13,10
            db  'nought.  The DOS box should END in two seconds, with a',13,10
            db  'reason; the MACHINE should not.',13,10,13,10,'$'
msg_nostuck db  'FAILED - the error was never reported',13,10,'$'
msg_mute    db  ' this coprocessor reports no errors',13,10,'$'
msg_byline  db  'The error comes in on interrupt line 13 here, once: there',13,10
            db  'is nothing to be stuck on.',13,10,'$'

t_there     db  '   1 a coprocessor answers                    $'
t_agree     db  '   2 ...and the kernel says the same          $'
t_add       db  '   3 it can add                               $'
t_mmx       db  '   4 MMX                                      $'
t_sse       db  '   5 SSE runs                                 $'
t_err       db  '   6 an unmasked error reaches INT 75h        $'
t_once      db  '   7 ...once: times, status, where  $'
t_fresh     db  '   8 a child starts with a fresh state        $'
t_mine      db  '   9 control word and stack survive it  $'
t_mxcsr     db  '  10 ...and MXCSR                        $'

prog_end    equ $

_FP     ENDS
        END     start
