;=====================================================================
; FPU32.ASM - FPU32.EXE, the coprocessor, MMX and SSE from a native
;             program.
;
; FPUTEST asks from inside a DOS box.  This asks from ring 3 of the
; kernel's own kind of program, which goes through the other half of
; everything: fn4B and fn4C keep the state and not vdm_push and
; vdm_pop, and an error nobody masked ends the program, because a
; native program has nowhere to be handed it.
;
;    1  the kernel says there is a coprocessor (21h/F1h AL=13h)
;    2  it can add
;    3  MMX, if the processor has it
;    4  SSE runs, if the processor has it
;    5  a native child starts with a fresh state
;    6  the control word and the stack are this program's own again
;       after it
;    7  ...and MXCSR
;    8  a DOS program as the child: the same two questions of
;       FPUTEST /C, which is a box started from here
;    9  ...and the state is this program's own again after that
;
; FPU32 /C is the child: it says what it was given in its exit code,
; leaves a mess and ends.
;
; FPU32 /F unmasks divide-by-zero and divides.  The program should be
; ended - "exception 10 ... floating point" - and the batch file that
; ran it should carry on.
;
; Assembled with -fpi87.
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        .387
        INCLUDE PMDOS.INC

_F3     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_F3, DS:_F3, ES:_F3, SS:_F3
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET f3_entry>,<0>,4096

CPU_MMX         EQU     00800000h       ; CPUID's EDX
CPU_SSE         EQU     02000000h
EFLAG_ID        EQU     00200000h

CW_RESET        EQU     037Fh           ; what FNINIT leaves
CW_UNMASK       EQU     037Bh           ; ...with divide-by-zero unmasked
CW_MINE         EQU     0F7Fh           ; the parent's: round towards zero
CW_CHILD        EQU     027Fh
MX_RESET        EQU     1F80h
MX_MINE         EQU     7F80h
MX_CHILD        EQU     9F80h

f3_entry:
        mov     esi,SLOT_PSP + 81h
        movzx   ecx,byte ptr ds:[SLOT_PSP + 80h]
f3_tail:
        or      ecx,ecx
        jz      f3_parent
        cmp     byte ptr ds:[esi],'/'
        jne     f3_tnext
        mov     al,ds:[esi+1]
        or      al,20h
        cmp     al,'c'
        je      f3_child
        cmp     al,'f'
        je      f3_fault
f3_tnext:
        inc     esi
        dec     ecx
        jmp     short f3_tail

;=====================================================================
; THE CHILD.  Looked at before anything is initialised.
;=====================================================================
f3_child:
        call    ask
        xor     bl,bl
        test    byte ptr [kflags],FPUF_X87
        jz      ch_exit
        fnstcw  word ptr [cw_got]
        fnstenv [envbuf]
        cmp     word ptr [cw_got],CW_RESET
        jne     ch_stale
        test    word ptr [envbuf+4],3800h       ; the status word: top of stack
        jnz     ch_stale
        cmp     word ptr [envbuf+8],0FFFFh      ; the tags: every one empty
        jne     ch_stale
        test    byte ptr [kflags],FPUF_SSE
        jz      ch_mess
        db      0Fh,0AEh,1Dh            ; STMXCSR [mx_got]
        dd      OFFSET mx_got
        cmp     dword ptr [mx_got],MX_RESET
        je      ch_mess
ch_stale:
        mov     bl,2
ch_mess:
        fninit
        mov     word ptr [cw_got],CW_CHILD
        fldcw   word ptr [cw_got]
        mov     ecx,8
ch_fill:
        fldz
        loop    ch_fill
        test    byte ptr [kflags],FPUF_SSE
        jz      ch_mmx
        mov     dword ptr [mx_got],MX_CHILD
        db      0Fh,0AEh,15h            ; LDMXCSR [mx_got]
        dd      OFFSET mx_got
ch_mmx:
        test    dword ptr [cpufeat],CPU_MMX
        jz      ch_exit
        mov     eax,12345678h
        db      0Fh,6Eh,0C0h            ; MOVD MM0,EAX - and no EMMS
ch_exit:
        mov     al,bl
        mov     ah,4Ch
        int     21h

;=====================================================================
; /F - an error nobody masked.
;=====================================================================
f3_fault:
        mov     edx,OFFSET msg_fault
        call    puts
        fninit
        mov     word ptr [cw_got],CW_UNMASK
        fldcw   word ptr [cw_got]
        fld1
        fldz
        fdivp   st(1),st
        fnstsw  word ptr [sw_got]       ; (which does not wait, or report)
        fwait
        fninit
        ; Still here.  A coprocessor with an error to report has its
        ; summary bit set; one that has not was never going to report
        ; anything, and DOSBox's is such a one.
        mov     edx,OFFSET msg_nofault
        test    word ptr [sw_got],0080h
        jnz     ff_say
        mov     edx,OFFSET msg_mute
ff_say:
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; THE PARENT.
;=====================================================================
f3_parent:
        mov     edx,OFFSET msg_hi
        call    puts
        call    ask

        mov     edx,OFFSET msg_kern
        call    puts
        cmp     byte ptr [kanswer],0
        jne     pa_kshow
        mov     edx,OFFSET msg_kdumb
        call    puts
        jmp     short pa_cpu
pa_kshow:
        movzx   eax,byte ptr [kflags]
        call    put2
        mov     edx,OFFSET msg_crlf
        call    puts
pa_cpu:
        mov     edx,OFFSET msg_cpu
        call    puts
        mov     eax,[cpufeat]
        call    put8
        mov     edx,OFFSET msg_crlf
        call    puts
        mov     edx,OFFSET msg_crlf
        call    puts

;--- 1 ------------------------------------------------------------------
        mov     edx,OFFSET t_there
        call    puts
        test    byte ptr [kflags],FPUF_X87
        jnz     t1_ok
        call    bad
        jmp     done
t1_ok:
        call    ok

;--- 2: it can add ------------------------------------------------------
        mov     edx,OFFSET t_add
        call    puts
        fninit
        mov     dword ptr [i1],2
        mov     dword ptr [i2],3
        fild    dword ptr [i1]
        fiadd   dword ptr [i2]
        fistp   dword ptr [i3]
        cmp     dword ptr [i3],5
        jne     t2_bad
        call    ok
        jmp     short t3
t2_bad:
        call    bad

;--- 3: MMX -------------------------------------------------------------
t3:
        mov     edx,OFFSET t_mmx
        call    puts
        test    dword ptr [cpufeat],CPU_MMX
        jnz     t3_go
        mov     edx,OFFSET msg_notcpu
        call    puts
        jmp     short t4
t3_go:
        mov     eax,00010203h
        db      0Fh,6Eh,0C0h            ; MOVD  MM0,EAX
        db      0Fh,0FEh,0C0h           ; PADDD MM0,MM0
        db      0Fh,7Eh,0C0h            ; MOVD  EAX,MM0
        db      0Fh,77h                 ; EMMS
        cmp     eax,00020406h
        jne     t3_bad
        call    ok
        jmp     short t4
t3_bad:
        call    bad

;--- 4: SSE -------------------------------------------------------------
; Not run unless the kernel says it may be: a native program has no
; handler to catch the invalid opcode with, and would simply be ended.
t4:
        mov     edx,OFFSET t_sse
        call    puts
        test    dword ptr [cpufeat],CPU_SSE
        jnz     t4_has
        mov     edx,OFFSET msg_notcpu
        call    puts
        jmp     short t5
t4_has:
        test    byte ptr [kflags],FPUF_SSE
        jz      t4_off
        db      0Fh,10h,05h             ; MOVUPS XMM0,[v_in]
        dd      OFFSET v_in
        db      0Fh,58h,0C0h            ; ADDPS  XMM0,XMM0
        db      0Fh,11h,05h             ; MOVUPS [v_out],XMM0
        dd      OFFSET v_out
        cmp     dword ptr [v_out],40000000h     ; 2.0
        jne     t4_bad
        cmp     dword ptr [v_out+12],41000000h  ; 8.0
        jne     t4_bad
        call    ok
        jmp     short t5
t4_off:
        mov     edx,OFFSET msg_off
        call    puts
t4_bad:
        call    bad

;--- 5 to 7: a native child ---------------------------------------------
t5:
        mov     ah,19h                  ; both children are on this disk
        int     21h
        add     al,'A'
        mov     [path32],al
        mov     [path16],al

        mov     edx,OFFSET path32
        call    spawn
        jc      t5_noexec

        mov     edx,OFFSET t_fresh
        call    puts
        mov     ah,4Dh
        int     21h
        or      al,al
        jnz     t5_bad
        call    ok
        jmp     short t6
t5_bad:
        call    bad
        jmp     short t6
t5_noexec:
        mov     edx,OFFSET t_fresh
        call    puts
        mov     edx,OFFSET msg_noexec
        call    puts
        call    bad
        jmp     t8

t6:
        mov     edx,OFFSET t_mine
        call    puts
        call    judge
        mov     edx,OFFSET t_mxcsr
        call    puts
        call    judgemx

;--- 8 and 9: a DOS program as the child --------------------------------
t8:
        mov     edx,OFFSET path16
        call    spawn
        jc      t8_noexec

        mov     edx,OFFSET t_fresh16
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
        mov     edx,OFFSET t_fresh16
        call    puts
        mov     edx,OFFSET msg_noexec
        call    puts
        call    bad
        jmp     short done
t9:
        mov     edx,OFFSET t_mine16
        call    puts
        call    judge

done:
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     failed
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
failed:
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; ask - what the kernel says and what CPUID says.  No coprocessor
;       instruction is executed here: the child comes through it too.
;=====================================================================
ask:
        mov     eax,0F113h
        int     21h
        jc      ask_cpu
        mov     [kflags],al
        mov     byte ptr [kanswer],1
ask_cpu:
        pushfd
        pop     eax
        mov     ecx,eax
        xor     eax,EFLAG_ID
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx
        popfd
        xor     eax,ecx
        test    eax,EFLAG_ID
        jz      ask_out
        xor     eax,eax
        db      0Fh,0A2h                ; CPUID
        or      eax,eax
        jz      ask_out
        mov     eax,1
        db      0Fh,0A2h
        mov     [cpufeat],edx
ask_out:
        ret

;---------------------------------------------------------------------
; spawn - EDX -> the program.  This program's state is set up, the
;         child is run with /C, and what is in the registers afterwards
;         is taken out of them before anything else can touch it.  CF
;         if the child could not be run.
;---------------------------------------------------------------------
spawn:
        fninit
        mov     word ptr [cw_got],CW_MINE
        fldcw   word ptr [cw_got]
        fldpi
        fld1
        test    byte ptr [kflags],FPUF_SSE
        jz      sp_exec
        mov     dword ptr [mx_got],MX_MINE
        db      0Fh,0AEh,15h            ; LDMXCSR [mx_got]
        dd      OFFSET mx_got
sp_exec:
        mov     dword ptr [pblk],0
        mov     dword ptr [pblk+4],OFFSET tail
        mov     ebx,OFFSET pblk
        mov     ax,4B00h
        int     21h
        jc      sp_fail

        fnstcw  word ptr [cw_got]
        fnstsw  word ptr [sw_got]
        mov     dword ptr [i1],0
        mov     dword ptr [i2],0
        fnclex
        fistp   dword ptr [i1]
        fistp   dword ptr [i2]
        fnstsw  word ptr [sw_after]
        mov     dword ptr [mx_got],0
        test    byte ptr [kflags],FPUF_SSE
        jz      sp_done
        db      0Fh,0AEh,1Dh            ; STMXCSR [mx_got]
        dd      OFFSET mx_got
sp_done:
        fninit
        clc
        ret
sp_fail:
        fninit
        stc
        ret

;---------------------------------------------------------------------
; judge - was that this program's state?
;---------------------------------------------------------------------
judge:
        movzx   eax,word ptr [cw_got]
        call    put4
        mov     dl,' '
        call    putc
        movzx   eax,word ptr [sw_got]
        call    put4
        cmp     word ptr [cw_got],CW_MINE
        jne     jd_bad
        mov     ax,[sw_got]             ; two on the stack: the top is 6
        and     ax,3800h
        cmp     ax,3000h
        jne     jd_bad
        cmp     dword ptr [i1],1
        jne     jd_bad
        cmp     dword ptr [i2],3        ; pi, towards zero
        jne     jd_bad
        test    word ptr [sw_after],0041h
        jnz     jd_bad
        call    ok
        ret
jd_bad:
        call    bad
        ret

judgemx:
        test    byte ptr [kflags],FPUF_SSE
        jnz     jm_go
        mov     edx,OFFSET msg_nosse
        call    puts
        ret
jm_go:
        mov     eax,[mx_got]
        call    put4
        cmp     dword ptr [mx_got],MX_MINE
        jne     jm_bad
        call    ok
        ret
jm_bad:
        call    bad
        ret

;=====================================================================
; helpers
;=====================================================================
ok:
        push    edx
        mov     edx,OFFSET msg_ok
        call    puts
        pop     edx
        ret
bad:
        push    edx
        mov     edx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     edx
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret
putc:
        push    eax
        push    edx
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        ret
put8:                                   ; EAX in hex
        push    eax
        shr     eax,16
        call    put4
        pop     eax
put4:
        push    eax
        shr     eax,8
        call    put2
        pop     eax
put2:
        push    eax
        shr     al,4
        call    put1
        pop     eax
        push    eax
        and     al,0Fh
        call    put1
        pop     eax
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
i1          dd  0
i2          dd  0
i3          dd  0
pblk        dd  0, 0
cw_got      dw  0
sw_got      dw  0
sw_after    dw  0
kflags      db  0
kanswer     db  0
fails       db  0
tail        db  3,' /C',0Dh
envbuf      db  32 dup(0)
path32      db  'C:\TESTS\FPU32.EXE',0
path16      db  'C:\TESTS\FPUTEST.COM',0

msg_hi      db  13,10,'FPU32 - the coprocessor, MMX and SSE in a native program',13,10,13,10,'$'
msg_kern    db  '  the kernel says (21h/F1h AL=13h)  $'
msg_kdumb   db  'nothing: it has no such call',13,10,'$'
msg_cpu     db  '  CPUID says                        $'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_notcpu  db  ' not in this processor',13,10,'$'
msg_nosse   db  ' no SSE here',13,10,'$'
msg_off     db  ' not switched on$'
msg_noexec  db  ' could not run the child$'
msg_pass    db  'FPU32: all checks passed',13,10,'$'
msg_fail    db  'FPU32: FAILURES - see above',13,10,'$'
msg_fault   db  'One over nought, with the error unmasked.  The program',13,10
            db  'should be ended, and the prompt should come back.',13,10,'$'
msg_nofault db  'FAILED - the error was never reported',13,10,'$'
msg_mute    db  'This coprocessor reports no errors.',13,10,'$'

t_there     db  '   1 the kernel says there is a coprocessor   $'
t_add       db  '   2 it can add                               $'
t_mmx       db  '   3 MMX                                      $'
t_sse       db  '   4 SSE runs                                 $'
t_fresh     db  '   5 a child starts with a fresh state        $'
t_mine      db  '   6 control word and stack survive it  $'
t_mxcsr     db  '   7 ...and MXCSR                        $'
t_fresh16   db  '   8 a DOS program as the child: the same     $'
t_mine16    db  '   9 control word and stack survive it  $'

_F3     ENDS
        END
