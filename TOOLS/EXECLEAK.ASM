;=====================================================================
; EXECLEAK.ASM - EXECLEAK.EXE, does a failed EXEC give its stack back?
;
; Every native program's DOS calls run on one kernel stack, and 4Bh
; moves r3_esp0 down to the caller's frame before it loads the child,
; so that the child's calls land below the parent's waiting ones.  A
; child that runs gives the level back in 4Ch.  A child that never
; loaded used not to: f4B_loadfail put back the slot, the heap and the
; coprocessor, and left r3_esp0 where it was - one INT 21h frame, 64
; bytes, below the caller's own.  Every failed EXEC took another 64.
;
; Nothing failed when it happened, which is why it lasted.  A shell
; looks a command up along PATH by trying it, so it fails two or three
; EXECs for every one it finds, and the kernel stack walked quietly
; down through the 14K it has, over the DPMI client's LDT and into the
; DPMI ring-0 stack under it, until whichever exception came next had
; nowhere to land.  The machine triple-faulted at around the hundred-
; and-fiftieth program a shell had run - RUNTESTS.PS1 found it, with a
; batch file that ran a small program between every test.
;
; So this fails a thousand of them, which with the leak is 64K of
; stack: through the IDT, and the machine resets long before the
; result is printed.  Without it the depth never moves and the last
; check - a real child, which has to load and run and say so - passes.
;
;   1  a thousand EXECs of a file that is not there all fail - with
;      2, or 8 on a machine with no room to park this program
;   2  and then a child that IS there runs: this program, as /C,
;      which ends with 42 for 4Dh to report
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_XL     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_XL, DS:_XL, ES:_XL, SS:_XL
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET xl_entry>,<0>,4096

NTRIES          EQU     1000
CHILD_CODE      EQU     42

xl_entry:
        ; "/C" in the tail: the child, which has nothing to do but end
        mov     esi,SLOT_PSP + 81h
        movzx   ecx,byte ptr ds:[SLOT_PSP + 80h]
xl_tail:
        or      ecx,ecx
        jz      xl_checks
        cmp     byte ptr ds:[esi],'/'
        jne     xl_tnext
        cmp     ecx,2
        jb      xl_checks
        mov     al,ds:[esi+1]
        or      al,20h
        cmp     al,'c'
        jne     xl_tnext
        mov     ax,4C00h + CHILD_CODE
        int     21h
xl_tnext:
        inc     esi
        dec     ecx
        jmp     short xl_tail

xl_checks:
        mov     edx,OFFSET msg_hi
        call    puts

;--- 1: a thousand that cannot load -----------------------------------
        mov     edx,OFFSET msg_t1
        call    puts
        mov     dword ptr [nran],0
        mov     dword ptr [nodd],0
        mov     ecx,NTRIES
t1_lp:
        push    ecx
        ; A REAL PARAMETER BLOCK, empty, and not EBX = 0.  A null EBX is
        ; refused as a pointer before 4Bh has done anything - access
        ; denied, 5 - so it never reached the path that leaked, and the
        ; first version of this test passed on the kernel it was
        ; written to catch.
        mov     edx,OFFSET path_none
        mov     ebx,OFFSET xempty
        mov     ax,4B00h
        int     21h
        pop     ecx
        jnc     t1_ran                  ; it found something to run
        cmp     ax,ERR_NOFILE
        je      t1_next
        cmp     ax,ERR_NOMEM
        je      t1_next
        cmp     dword ptr [nodd],0      ; failed, but with something else:
        jne     t1_odd                  ; the first one is the one to know
        mov     [got],ax
        mov     eax,NTRIES + 1
        sub     eax,ecx
        mov     [gotat],eax
t1_odd:
        inc     dword ptr [nodd]
        jmp     short t1_next
t1_ran:
        inc     dword ptr [nran]
t1_next:
        loop    t1_lp
        cmp     dword ptr [nran],0
        jne     t1_bad
        cmp     dword ptr [nodd],0
        jne     t1_bad
        call    good
        jmp     short t2
t1_bad:
        call    bad
        mov     edx,OFFSET msg_got
        call    puts
        movzx   eax,word ptr [got]
        call    putdec
        mov     edx,OFFSET msg_gotat
        call    puts
        mov     eax,[gotat]
        call    putdec
        mov     edx,OFFSET msg_gotn
        call    puts
        mov     eax,[nodd]
        call    putdec
        mov     edx,OFFSET msg_gotran
        call    puts
        mov     eax,[nran]
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts

;--- 2: and one that can ----------------------------------------------
t2:
        mov     edx,OFFSET msg_t2
        call    puts
        mov     edx,OFFSET path_self
        mov     ebx,OFFSET xblock
        mov     ax,4B00h
        int     21h
        jc      t2_bad
        mov     ah,4Dh
        int     21h
        cmp     al,CHILD_CODE
        jne     t2_bad
        call    good
        jmp     short wrapup
t2_bad:
        call    bad

;=====================================================================
wrapup:
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET results
        call    puts
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     wr_fail
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
wr_fail:
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
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

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; putdec - EAX in decimal
putdec:
        pushad
        mov     ecx,10
        xor     ebx,ebx
pd_div:
        xor     edx,edx
        div     ecx
        push    edx
        inc     ebx
        or      eax,eax
        jnz     pd_div
pd_out:
        pop     edx
        add     dl,'0'
        mov     ah,02h
        int     21h
        dec     ebx
        jnz     pd_out
        popad
        ret

;=====================================================================
msg_hi      db  'EXECLEAK - does a failed EXEC give its stack back?',13,10,13,10,'$'
msg_t1      db  ' 1 a thousand EXECs of nothing all fail  ','$'
msg_t2      db  ' 2 ...and a real child still runs: 42    ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_got     db  '     an EXEC said: ','$'
msg_gotat   db  ', first at try ','$'
msg_gotn    db  ', and so did ','$'
msg_gotran  db  '; tries that ran something: ','$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'EXECLEAK: all checks passed',13,10,'$'
msg_fail    db  'EXECLEAK: FAILURES',13,10,'$'

path_none   db  'C:\NOSUCH.EXE',0
path_self   db  'C:\TESTS\EXECLEAK.EXE',0
xempty      dd  0                       ; no environment
            dd  0                       ; and no tail
xblock      dd  0                       ; no environment
            dd  OFFSET child_tail
child_tail  db  3,' /C',0Dh

fails       db  0
got         dw  0
nran        dd  0
nodd        dd  0
gotat       dd  0
resptr      dd  OFFSET results
results     db  16 dup('$')

_XL     ENDS
        END
