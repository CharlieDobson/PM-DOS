;=====================================================================
; IRQTEST.ASM - IRQTEST.EXE, two handlers on one interrupt line.
;
; A PCI line is shared, so a line in this kernel has a chain of
; handlers (IO\IRQ.INC): all of them are called for every interrupt
; and each says whether it was its own device asking.  Nothing in the
; kernel uses a shared line yet, so there is nothing for a test to
; watch - unless the kernel was built to bring its own.  BUILD irqtest
; (-dIRQ_SELFTEST) puts two handlers on the real-time clock's line,
; one that never claims an interrupt and one that claims the clock's,
; and this program reads what they counted.
;
; On any other kernel it says so and tests nothing.
;
; Seven checks:
;
;   1  the timer's line refused a handler: it is the kernel's alone
;   2  a line that was full refused one more
;   3  a line given back its handlers had none left, and was shut
;   4  both handlers are on line 8, and the kernel says it is hooked
;   5  the clock interrupts, and its handler claims every one
;   6  the silent handler was called every time too
;   7  nothing was left unclaimed, and nothing went to a DOS box
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_IT     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_IT, DS:_IT, ES:_IT, SS:_IT
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET it_entry>,<0>,4096

BUS     MACRO   fn
        mov     eax,0F112h + (fn SHL 16)
        int     21h
        ENDM

it_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        BUS     BUS_SELFTEST
        jc      it_none
        cmp     eax,54515249h           ; 'IRQT'
        jne     it_none
        mov     [quiet0],ebx
        mov     [rtc0],ecx
        mov     [found],edx

        mov     edx,OFFSET msg_t1
        mov     ecx,1
        call    flagged
        mov     edx,OFFSET msg_t2
        mov     ecx,2
        call    flagged
        mov     edx,OFFSET msg_t3
        mov     ecx,4
        call    flagged

        mov     edx,OFFSET msg_t4
        call    puts
        test    dword ptr [found],8
        jz      t4_bad
        mov     edx,8
        BUS     BUS_IRQSTAT
        jc      t4_bad
        test    eax,IRQF_HOOKED
        jz      t4_bad
        mov     [lost0],edx
        mov     [box0],ecx
        call    good
        jmp     short t5
t4_bad:
        call    bad
        jmp     finish

;--- a second, by the clock DOS keeps
t5:
        call    second
        BUS     BUS_SELFTEST
        mov     [quiet1],ebx
        mov     [rtc1],ecx
        mov     edx,8
        BUS     BUS_IRQSTAT
        mov     [lost1],edx
        mov     [box1],ecx

        mov     edx,OFFSET msg_t5
        call    puts
        mov     eax,[rtc1]
        sub     eax,[rtc0]
        call    pdec
        cmp     eax,32                  ; sixty-four a second, give or take
        jb      t5_bad                  ; where in a second it started
        cmp     eax,200
        ja      t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad
t6:
        mov     edx,OFFSET msg_t6
        call    puts
        mov     eax,[quiet1]
        sub     eax,[quiet0]
        call    pdec
        mov     ecx,[rtc1]
        sub     ecx,[rtc0]
        sub     eax,ecx                 ; an interrupt between the two
        cmp     eax,1                   ; readings is one apart, either way
        jle     t6_near
        jmp     short t6_bad
t6_near:
        cmp     eax,-1
        jl      t6_bad
        call    good
        jmp     short t7
t6_bad:
        call    bad
t7:
        mov     edx,OFFSET msg_t7
        call    puts
        mov     eax,[lost1]
        sub     eax,[lost0]
        jnz     t7_bad
        mov     eax,[box1]
        sub     eax,[box0]
        jnz     t7_bad
        call    good
        jmp     short finish
t7_bad:
        call    bad
        jmp     short finish

it_none:
        mov     byte ptr [skipped],1
        mov     edx,OFFSET msg_none
        call    puts

finish:
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET results
        call    puts
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        je      fin_ok
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h
fin_ok:
        cmp     byte ptr [skipped],0
        je      fin_pass
        mov     edx,OFFSET msg_skipped
        call    puts
        mov     ax,4C00h
        int     21h
fin_pass:
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h

;--- flagged - EDX -> what is being asked, ECX = the bit of [found]
;--- that answers it
flagged:
        call    puts
        test    dword ptr [found],ecx
        jz      bad
        jmp     short good

;--- second - wait for the second to change, and then for it to change
;--- again: one whole one
second:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     bh,2
        mov     ah,2Ch
        int     21h
        mov     bl,dh
sec_lp:
        mov     ah,2Ch
        int     21h
        cmp     dh,bl
        je      sec_lp
        mov     bl,dh
        dec     bh
        jnz     sec_lp
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

pdec:
        pushad
        mov     edi,OFFSET decbuf + 11
        mov     byte ptr [edi],'$'
        mov     ebx,10
pd_lp:
        xor     edx,edx
        div     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        or      eax,eax
        jnz     pd_lp
        mov     edx,edi
        call    puts
        mov     edx,OFFSET msg_sp
        call    puts
        popad
        ret

good:
        push    edx
        mov     dl,'.'
        call    tally
        mov     edx,OFFSET msg_ok
        call    puts
        pop     edx
        ret
bad:
        push    edx
        mov     dl,'X'
        call    tally
        mov     edx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     edx
        ret
tally:
        push    ebx
        mov     ebx,[resptr]
        mov     [ebx],dl
        inc     ebx
        mov     byte ptr [ebx],'$'
        mov     [resptr],ebx
        pop     ebx
        ret

;=====================================================================
msg_hi      db  'IRQTEST - two handlers on one interrupt line',13,10,13,10,'$'
msg_t1      db  ' 1 the timer''s line takes no handler      ','$'
msg_t2      db  ' 2 a full line takes no more              ','$'
msg_t3      db  ' 3 an emptied line is shut                ','$'
msg_t4      db  ' 4 two handlers on line 8                 ','$'
msg_t5      db  ' 5 the clock''s, claimed in 1s        ','$'
msg_t6      db  ' 6 the silent one, called            ','$'
msg_t7      db  ' 7 none unclaimed, none to a box          ','$'
msg_none    db  '  This kernel was not built with the self-test',13,10
            db  '  (BUILD irqtest): there are no handlers to count.',13,10,'$'
msg_sp      db  ' ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_skipped db  'IRQTEST: nothing on this kernel to test',13,10,'$'
msg_pass    db  'IRQTEST: all checks passed',13,10,'$'
msg_fail    db  'IRQTEST: FAILURES',13,10,'$'

found       dd  0
quiet0      dd  0
quiet1      dd  0
rtc0        dd  0
rtc1        dd  0
lost0       dd  0
lost1       dd  0
box0        dd  0
box1        dd  0
skipped     db  0
fails       db  0
            db  0,0
decbuf      db  12 dup(0)
resptr      dd  OFFSET results
results     db  16 dup('$')

_IT     ENDS
        END
