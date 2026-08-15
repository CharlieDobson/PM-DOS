;=====================================================================
; R3TEST.ASM - R3TEST.EXE: is a native program actually at ring 3, and
;              does the kernel still work from there?
;
; The permanent test for the ring-3 release.  CRASH proves what a
; program can no longer DO; this proves that everything it is still
; supposed to do still works, which is the half that a protection
; change quietly breaks.
;
; Fourteen checks:
;    1  CS is SEL_PROG_CS and its RPL is 3 - we are unprivileged
;    2  SS and DS are the program's data selector, not the kernel's
;    3  the PSP is at SLOT_PSP, in this program's own terms
;    4  PSP+0 is the INT 20h a RET is supposed to land on
;    5  the parent field is zero: started by something out of reach
;    6  PSP_MEMTOP is the slot size
;    7  the DTA starts at PSP+80h, as an offset
;    8  a DTA set and read back survives as an offset
;    9  a null pointer handed to DOS is refused, not followed
;   10  a pointer past the end of the slot is refused
;   11  48h allocates INSIDE the slot, above this image
;   12  what 48h returns can be written, freed, and re-allocated
;   13  25h and 35h round-trip a handler of ours
;   14  a critical error reaches a handler at ring 3 and its answer
;       reaches the kernel - which is the whole call-out mechanism,
;       and the only check here that exercises it
;
; Check 14 needs a drive that will fail, and uses B:.  A PC has a
; second floppy letter whether or not it has the drive, so the read
; fails on every machine this has ever run on - and it fails in the one
; way that raises INT 24h rather than the one that answers "invalid
; drive" without asking anybody.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_R3     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_R3, DS:_R3, ES:_R3, SS:_R3
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET r3_entry>,<0>,4096

r3_entry:
        mov     edx,OFFSET msg_hi
        call    puts

;--- 1. what ring is this? -------------------------------------------
; The RPL of CS is the current privilege level, and reading it is the
; only way a program can ask.  Three means the CPU is enforcing every
; other check in this file; zero would mean the program had been
; started the old way and nothing below proves anything.
        mov     edx,OFFSET t1
        call    say
        xor     eax,eax
        mov     ax,cs
        cmp     ax,SEL_PROG_CS
        jne     t1_no
        test    al,3                    ; RPL 3, and CPL follows CS's RPL
        jz      t1_no
        call    ok
        jmp     short t2
t1_no:
        call    bad

;--- 2. and the data selectors ---------------------------------------
t2:
        mov     edx,OFFSET t2m
        call    say
        mov     ax,ds
        cmp     ax,SEL_PROG_DS
        jne     t2_no
        mov     ax,ss
        cmp     ax,SEL_PROG_DS
        jne     t2_no
        mov     ax,es
        cmp     ax,SEL_PROG_DS
        jne     t2_no
        call    ok
        jmp     short t3
t2_no:
        call    bad

;--- 3. where the PSP is ---------------------------------------------
t3:
        mov     edx,OFFSET t3m
        call    say
        mov     ah,62h
        int     21h
        cmp     ebx,SLOT_PSP
        jne     t3_no
        call    ok
        jmp     short t4
t3_no:
        call    bad

;--- 4. and what is at the front of it -------------------------------
t4:
        mov     edx,OFFSET t4m
        call    say
        cmp     word ptr ds:[SLOT_PSP + PSP_INT20],20CDh
        jne     t4_no
        call    ok
        jmp     short t5
t4_no:
        call    bad

;--- 5. the parent, which is out of reach and says so ----------------
t5:
        mov     edx,OFFSET t5m
        call    say
        cmp     dword ptr ds:[SLOT_PSP + PSP_PARENT],0
        jne     t5_no
        call    ok
        jmp     short t6
t5_no:
        call    bad

;--- 6. how much memory the slot is ----------------------------------
t6:
        mov     edx,OFFSET t6m
        call    say
        cmp     dword ptr ds:[SLOT_PSP + PSP_MEMTOP],PROG_SIZE
        jne     t6_no
        call    ok
        jmp     short t7
t6_no:
        call    bad

;--- 7. the DTA, which the kernel holds flat and answers as an offset -
t7:
        mov     edx,OFFSET t7m
        call    say
        mov     ah,2Fh
        int     21h
        cmp     ebx,SLOT_PSP + PSP_TAIL
        jne     t7_no
        call    ok
        jmp     short t8
t7_no:
        call    bad

;--- 8. ...and one of ours, there and back ---------------------------
t8:
        mov     edx,OFFSET t8m
        call    say
        mov     edx,OFFSET dta
        mov     ah,1Ah
        int     21h
        mov     ah,2Fh
        int     21h
        cmp     ebx,OFFSET dta
        jne     t8_no
        call    ok
        jmp     short t9
t8_no:
        call    bad

;--- 9. a null pointer, which DOS must not follow --------------------
; The guard page catches this program dereferencing zero.  This is the
; other half: zero handed to the kernel, which is at ring 0 and could
; follow it perfectly well.  It has to refuse instead.
t9:
        mov     edx,OFFSET t9m
        call    say
        xor     edx,edx
        mov     ax,3D00h                ; open the file named by nothing
        int     21h
        jnc     t9_no                   ; it must not have succeeded
        cmp     ax,ERR_ACCESS
        jne     t9_no
        call    ok
        jmp     short t10
t9_no:
        call    bad

;--- 10. and an offset past the end of the slot ----------------------
t10:
        mov     edx,OFFSET t10m
        call    say
        mov     edx,PROG_SIZE + 1000h
        mov     ax,3D00h
        int     21h
        jnc     t10_no
        cmp     ax,ERR_ACCESS
        jne     t10_no
        call    ok
        jmp     short t11
t10_no:
        call    bad

;--- 11. 48h, which now comes out of the slot ------------------------
; The two bounds are the point.  Below the end of this program would
; mean the heap was laid over the image; at or above the slot size
; would mean the kernel had handed out an address from its own arena,
; which is the answer this program could not use.
t11:
        mov     edx,OFFSET t11m
        call    say
        mov     ebx,4096
        mov     ah,48h
        int     21h
        jc      t11_no
        mov     [blk1],eax
        cmp     eax,OFFSET heap_floor
        jbe     t11_no
        cmp     eax,PROG_SIZE
        jae     t11_no
        call    ok
        jmp     short t12
t11_no:
        call    bad

;--- 12. and it is real memory, freeable, and given out again --------
t12:
        mov     edx,OFFSET t12m
        call    say
        mov     eax,[blk1]
        or      eax,eax
        jz      t12_no
        mov     ebx,eax
        mov     dword ptr [ebx],12345678h       ; write it
        mov     dword ptr [ebx+4092],90ABCDEFh  ; and the far end of it
        cmp     dword ptr [ebx],12345678h
        jne     t12_no
        cmp     dword ptr [ebx+4092],90ABCDEFh
        jne     t12_no
        mov     edx,eax                 ; free it
        mov     ah,49h
        int     21h
        jc      t12_no
        mov     ebx,4096                ; and ask for it back
        mov     ah,48h
        int     21h
        jc      t12_no
        cmp     eax,[blk1]              ; first fit: the same block
        jne     t12_no
        call    ok
        jmp     short t13
t12_no:
        call    bad

;--- 13. a vector of ours, set and read back -------------------------
t13:
        mov     edx,OFFSET t13m
        call    say
        mov     ah,35h                  ; what is there now
        mov     al,23h
        int     21h
        mov     [old23],ebx
        mov     edx,OFFSET my23
        mov     ax,2523h
        int     21h
        mov     ah,35h
        mov     al,23h
        int     21h
        cmp     ebx,OFFSET my23
        jne     t13_no
        mov     edx,[old23]             ; and put back what was there,
        mov     ax,2523h                ; which is a KERNEL address and
        int     21h                     ; has to be recognised as one
        mov     ah,35h
        mov     al,23h
        int     21h
        cmp     ebx,[old23]
        jne     t13_no
        call    ok
        jmp     short t14
t13_no:
        call    bad

;--- 14. the whole call-out: DOS enters a handler at ring 3 ----------
; A critical error is raised inside the kernel, at ring 0, several
; calls deep in the disk layer.  Reaching this program's handler from
; there means building a frame on this program's stack and IRETing to
; it; the handler ends with the IRET its specification demands and
; lands on the trampoline in the PSP, which traps back into the kernel
; with the answer still in EAX.  If any part of that is wrong the
; machine does not come back at all, so a printed result is most of
; the test.
t14:
        mov     edx,OFFSET t14m
        call    say
        mov     ah,35h
        mov     al,24h
        int     21h
        mov     [old24],ebx
        mov     edx,OFFSET my24
        mov     ax,2524h
        int     21h

        mov     byte ptr [ran24],0
        mov     edx,OFFSET path_b       ; a drive that will not read
        mov     ah,4Eh
        mov     ecx,0
        int     21h

        push    eax                     ; the answer, whatever it was
        pushfd
        mov     edx,[old24]             ; our handler goes away FIRST:
        mov     ax,2524h                ; anything below here that fails
        int     21h                     ; must not re-enter it
        popfd
        pop     eax

        cmp     byte ptr [ran24],0
        je      t14_no                  ; the handler never ran
        cmp     byte ptr [ran24],1
        jne     t14_no                  ; it ran more than once
        call    ok
        jmp     short t_done
t14_no:
        call    bad

t_done:
        mov     edx,OFFSET msg_tot
        call    puts
        movzx   eax,byte ptr [n_ok]
        call    putdec
        mov     edx,OFFSET msg_of
        call    puts
        movzx   eax,byte ptr [n_run]
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts

        movzx   eax,byte ptr [n_ok]
        movzx   ebx,byte ptr [n_run]
        cmp     eax,ebx
        jne     t_fail
        mov     ax,4C00h
        int     21h
t_fail:
        mov     ax,4C01h
        int     21h

;=====================================================================
; Our INT 23h handler.  Never called here - check 13 only puts its
; address in and takes it out again - but it has to be a real one,
; because a handler that was never installed would prove nothing about
; what 35h answered.
;=====================================================================
my23:
        iretd

;=====================================================================
; Our INT 24h handler.  Answer FAIL, which is the answer that lets the
; DOS call return an error rather than terminating this program.
;
; It runs at ring 3, on this program's own stack, and it ends with the
; IRET its specification demands - which is the whole thing being
; tested.  It touches nothing that would need saving beyond the one
; byte it sets.
;=====================================================================
my24:
        inc     byte ptr [ran24]
        mov     eax,CERR_FAIL
        iretd

;=====================================================================
; Reporting
;=====================================================================
say:
        push    eax
        inc     byte ptr [n_run]
        mov     ah,09h
        int     21h
        pop     eax
        ret

ok:
        push    eax
        push    edx
        inc     byte ptr [n_ok]
        mov     edx,OFFSET msg_ok
        mov     ah,09h
        int     21h
        pop     edx
        pop     eax
        ret

bad:
        push    eax
        push    edx
        mov     edx,OFFSET msg_bad
        mov     ah,09h
        int     21h
        pop     edx
        pop     eax
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; EAX in decimal, no padding
putdec:
        pushad
        mov     edi,OFFSET numbuf + 11
        mov     byte ptr [edi],'$'
        mov     ecx,10
pd_lp:
        xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        or      eax,eax
        jnz     pd_lp
        mov     edx,edi
        mov     ah,09h
        int     21h
        popad
        ret

;=====================================================================
; Data
;=====================================================================
n_run       db  0
n_ok        db  0
ran24       db  0
            db  0
blk1        dd  0
old23       dd  0
old24       dd  0
dta         db  128 dup(0)
numbuf      db  12 dup(0)
path_b      db  'B:\*.*',0

msg_hi      db  13,10,'R3TEST - a native program, and where it is standing.',13,10,13,10,'$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_tot     db  13,10,'  passed: $'
msg_of      db  ' of $'
msg_crlf    db  13,10,'$'

t1          db  '   1  running at ring 3, in SEL_PROG_CS ....... $'
t2m         db  '   2  DS, ES and SS are the slot selector ..... $'
t3m         db  '   3  the PSP is at offset 1000h .............. $'
t4m         db  '   4  and it starts with INT 20h ............... $'
t5m         db  '   5  the parent is out of reach and says 0 .... $'
t6m         db  '   6  memory top is the size of the slot ....... $'
t7m         db  '   7  the DTA starts at PSP+80h ................ $'
t8m         db  '   8  a DTA of ours survives as an offset ...... $'
t9m         db  '   9  a null pointer is refused by DOS ......... $'
t10m        db  '  10  so is one past the end of the slot ....... $'
t11m        db  '  11  48h allocates inside the slot ............ $'
t12m        db  '  12  that memory works, frees and comes back .. $'
t13m        db  '  13  25h and 35h round-trip our handler ....... $'
t14m        db  '  14  INT 24h reaches a handler at ring 3 ...... $'

; The last thing in the image, so that check 11 can say the heap
; starts above it.  It has to be a defined byte and not a label at the
; very end, because a BSS of zero means the image ends where the file
; does and that is where the heap begins.
heap_floor  db  0

_R3     ENDS
        END
