;=====================================================================
; DPMIFPU.ASM - DPMIFPU.COM, the coprocessor's error in a DPMI client.
;
; FPUTEST asks whether a DOS box is told about an error it unmasked.
; A DPMI client is a DOS box that is somewhere else, and it is told by
; a different piece of the host: the fault arrives from ring 3 of
; protected mode, PMIO has nothing to emulate, and the DOS side - which
; has the client's vectors - sends it to the client's own handler on
; INT 75h, or to the real-mode one if the client has none (DPMI.INC,
; dfa_fperr).  Every extender's run-time library has one or the other.
;
;    1  the switch
;    2  the coprocessor can add, from protected mode
;    3  SSE runs there, if the kernel says it may be used
;    4  an unmasked error reaches the handler 0205h put on INT 75h
;    5  ...once, with divide-by-zero in the status word
;    6  with no handler of the client's, the real-mode one gets it
;    7  ...once
;
; Assembled twice, as DPMIVEC is: with -dCLIENT32 it is DPMIFP32.COM, a
; 32-bit client, whose handlers are given frames of dwords.
;
; Assembled with -fpi87.
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
        .387
_DF     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DF, DS:_DF, ES:_DF, SS:_DF
        ORG     0

IFDEF CLIENT32
PMIRET          MACRO
        db      66h
        iret
                ENDM
ELSE
PMIRET          MACRO
        iret
                ENDM
ENDIF

KF_SSE          EQU     04h             ; FPUF_SSE, from 21h/F1h AL=13h
CW_UNMASK       EQU     037Bh           ; divide-by-zero unmasked
VEC_FP          EQU     75h

start:
        mov     ah,4Ah                  ; a .COM is given all the memory
        mov     bx,1000h                ; there is, and the host wants some
        int     21h

        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results
        mov     [rm_seg],cs

        mov     ax,0F113h               ; asked here, in real mode, where
        int     21h                     ; the answer is a plain register
        jc      t1
        mov     [kflags],al

;=====================================================================
; 1 - into protected mode
;=====================================================================
t1:
        mov     dx,OFFSET msg_t1
        call    puts
        mov     ax,1687h
        int     2Fh
        or      ax,ax
        jnz     t1_none
        mov     [entry_off],di
        mov     [entry_seg],es
        push    cs
        pop     es
        mov     bx,si
        or      bx,bx
        jz      t1_switch
        mov     ah,48h
        int     21h
        jc      t1_none
        mov     es,ax
t1_switch:
IFDEF CLIENT32
        mov     ax,1                    ; a 32-bit client
ELSE
        xor     ax,ax                   ; a 16-bit client
ENDIF
        call    dword ptr [entry_off]
        jc      t1_none
        mov     [pm_ds],ds
        mov     [pm_cs],cs
        push    ds
        pop     es
        call    good
        jmp     short t2
t1_none:
        push    cs
        pop     es
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_nodpmi
        call    puts
        jmp     finish

;=====================================================================
; 2 - it can add
;=====================================================================
t2:
        mov     dx,OFFSET msg_t2
        call    puts
        fninit
        mov     word ptr [i1],2
        mov     word ptr [i2],3
        fild    word ptr [i1]
        fiadd   word ptr [i2]
        fistp   word ptr [i3]
        cmp     word ptr [i3],5
        jne     t2_bad
        call    good
        jmp     short t3
t2_bad:
        call    bad

;=====================================================================
; 3 - SSE.  Not run unless the kernel says it may be: the client has no
;     handler on the invalid opcode and the host has no call to give it
;     one with.
;=====================================================================
t3:
        mov     dx,OFFSET msg_t3
        call    puts
        test    byte ptr [kflags],KF_SSE
        jnz     t3_go
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_nosse
        call    puts
        jmp     short t4
t3_go:
        db      0Fh,10h,06h             ; MOVUPS XMM0,[v_in]
        dw      OFFSET v_in
        db      0Fh,58h,0C0h            ; ADDPS  XMM0,XMM0
        db      0Fh,11h,06h             ; MOVUPS [v_out],XMM0
        dw      OFFSET v_out
        cmp     dword ptr [v_out],40000000h     ; 2.0
        jne     t3_bad
        cmp     dword ptr [v_out+12],41000000h  ; 8.0
        jne     t3_bad
        call    good
        jmp     short t4
t3_bad:
        call    bad

;=====================================================================
; 4 and 5 - a handler of the client's own
;=====================================================================
t4:
        mov     dx,OFFSET msg_t4
        call    puts
        mov     ax,0204h
        mov     bl,VEC_FP
        int     31h
        mov     [old75],dx
        mov     [old75+2],cx
        mov     ax,0205h
        mov     bl,VEC_FP
        mov     cx,[pm_cs]
        mov     dx,OFFSET pm75
        movzx   edx,dx
        int     31h
        jc      t4_bad

        mov     [spwas],sp
        call    divide
        cmp     sp,[spwas]
        jne     t4_bad
        cmp     word ptr [npm],0
        jne     t4_ok

        ; Nothing arrived - and if the coprocessor had nothing to
        ; report, nothing was going to.  See FPUTEST, t6_none.
        test    word ptr [swpre],0080h
        jnz     t4_bad
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_mute
        call    puts
        mov     ax,0205h
        mov     bl,VEC_FP
        mov     cx,[old75+2]
        mov     dx,[old75]
        movzx   edx,dx
        int     31h
        jmp     finish
t4_ok:
        call    good
        jmp     short t5
t4_bad:
        mov     sp,[spwas]
        call    bad

t5:
        mov     dx,OFFSET msg_t5
        call    puts
        mov     ax,[npm]
        call    put4
        mov     ax,[swpm]
        call    put4
        cmp     word ptr [npm],1
        jne     t5_bad
        test    word ptr [swpm],0004h   ; ZE
        jz      t5_bad
        cmp     word ptr [nrm],0        ; and nobody else was told
        jne     t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad

;=====================================================================
; 6 and 7 - and without one.  The client's vector goes back to the
;     default and the real-mode vector gets a handler.
;=====================================================================
t6:
        mov     ax,0205h
        mov     bl,VEC_FP
        mov     cx,[old75+2]
        mov     dx,[old75]
        movzx   edx,dx
        int     31h

        mov     dx,OFFSET msg_t6
        call    puts
        mov     ax,0200h
        mov     bl,VEC_FP
        int     31h
        mov     [rmold75],dx
        mov     [rmold75+2],cx
        mov     ax,0201h
        mov     bl,VEC_FP
        mov     cx,[rm_seg]
        mov     dx,OFFSET rm75
        movzx   edx,dx
        int     31h

        mov     [spwas],sp
        call    divide
        cmp     sp,[spwas]
        jne     t6_bad
        cmp     word ptr [nrm],0
        je      t6_bad
        call    good
        jmp     short t7
t6_bad:
        mov     sp,[spwas]
        call    bad

t7:
        mov     dx,OFFSET msg_t7
        call    puts
        mov     ax,[nrm]
        call    put4
        mov     ax,[swrm]
        call    put4
        cmp     word ptr [nrm],1
        jne     t7_bad
        test    word ptr [swrm],0004h
        jz      t7_bad
        cmp     word ptr [npm],1        ; as it was
        jne     t7_bad
        call    good
        jmp     short t8
t7_bad:
        call    bad
t8:
        mov     ax,0201h
        mov     bl,VEC_FP
        mov     cx,[rmold75+2]
        mov     dx,[rmold75]
        movzx   edx,dx
        int     31h

finish:
        mov     dx,OFFSET msg_res
        call    puts
        mov     dx,OFFSET results
        call    puts
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

;=====================================================================
; divide - one over nought, with the error unmasked, and the FWAIT it
;          is reported at.  A handler has cleared it by the time this
;          returns, or this does not return.
;=====================================================================
divide:
        fninit
        mov     word ptr [cwtmp],CW_UNMASK
        fldcw   word ptr [cwtmp]
        fld1
        fldz
        fdivp   st(1),st
        fnstsw  word ptr [swpre]        ; (which does not wait, or report)
        fwait
        nop
        fninit
        ret

;=====================================================================
; The handlers.  Each looks, clears, and tells both interrupt
; controllers it has finished - which is what a run-time library's
; does, because on the machine it was written for it had to.
;=====================================================================
pm75:
        push    ds
        push    ax
        mov     ax,cs:[pm_ds]
        mov     ds,ax
        inc     word ptr [npm]
        fnstsw  word ptr [swpm]
        fnclex
        xor     al,al
        out     0F0h,al
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ax
        pop     ds
        sti
        PMIRET

rm75:
        push    ax
        inc     word ptr cs:[nrm]
        fnstsw  word ptr cs:[swrm]
        fnclex
        xor     al,al
        out     0F0h,al
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ax
        iret

;=====================================================================
good:
        push    dx
        mov     dl,'.'
        call    tally
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dl,'X'
        call    tally
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret
tally:
        push    bx
        mov     bx,[resptr]
        mov     [bx],dl
        inc     bx
        mov     byte ptr [bx],'$'
        mov     [resptr],bx
        pop     bx
        ret

puts:
        push    ax
        movzx   edx,dx                  ; DS:EDX, to a 32-bit client
        mov     ah,09h
        int     21h
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
put4:                                   ; AX in hex, and a space
        push    ax
        push    cx
        push    dx
        mov     cx,4
p4_lp:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p4_d
        add     al,7
p4_d:
        mov     dl,al
        call    putc
        pop     ax
        loop    p4_lp
        mov     dl,' '
        call    putc
        pop     dx
        pop     cx
        pop     ax
        ret

;=====================================================================
v_in        dd  3F800000h, 40000000h, 40400000h, 40800000h  ; 1 2 3 4
v_out       dd  4 dup(0)
entry_off   dw  0
entry_seg   dw  0
rm_seg      dw  0
pm_ds       dw  0
pm_cs       dw  0
old75       dw  0,0                     ; offset, then selector
rmold75     dw  0,0                     ; offset, then segment
npm         dw  0
nrm         dw  0
swpm        dw  0
swrm        dw  0
swpre       dw  0
spwas       dw  0
cwtmp       dw  0
i1          dw  0
i2          dw  0
i3          dw  0
resptr      dw  0
kflags      db  0
fails       db  0
results     db  12 dup('$')

IFDEF CLIENT32
msg_hi      db  13,10,'DPMIFP32 - the coprocessor''s error in a 32-bit '
            db  'DPMI client',13,10,13,10,'$'
ELSE
msg_hi      db  13,10,'DPMIFPU - the coprocessor''s error in a DPMI '
            db  'client',13,10,13,10,'$'
ENDIF
msg_t1      db  ' 1 into protected mode                   $'
msg_t2      db  ' 2 it can add                            $'
msg_t3      db  ' 3 SSE runs                              $'
msg_t4      db  ' 4 an error reaches our handler          $'
msg_t5      db  ' 5 ...once: times, status     $'
msg_t6      db  ' 6 with none, the real-mode one          $'
msg_t7      db  ' 7 ...once: times, status     $'
msg_nodpmi  db  'no DPMI host (skipped)',13,10,'$'
msg_nosse   db  'no SSE here',13,10,'$'
msg_mute    db  'this coprocessor reports no errors',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
IFDEF CLIENT32
msg_pass    db  'DPMIFP32: all checks passed',13,10,'$'
msg_fail    db  'DPMIFP32: FAILURES',13,10,'$'
ELSE
msg_pass    db  'DPMIFPU: all checks passed',13,10,'$'
msg_fail    db  'DPMIFPU: FAILURES',13,10,'$'
ENDIF

_DF     ENDS
        END     start
