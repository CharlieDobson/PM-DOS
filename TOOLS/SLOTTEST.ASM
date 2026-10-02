;=====================================================================
; SLOTTEST.ASM - SLOTTEST.EXE, every EXEC level in a slot of its own
;
; Until v0.62 every native program below the shell ran in ONE slot, at
; PROG_SLOT, and 4Bh made room for a child by copying its parent out
; into the arena, half a megabyte, and 4Ch copied it back.  Every level
; has a slot of its own out of the arena now (RING3.INC, slot_new),
; with a guard page of its own at offset zero, and gives it back when
; it ends (slot_free).  This checks what that has to mean:
;
;   1  programs nest until EXEC_MAX or the arena says no, and each one
;      finds its own memory as it left it when its child comes back
;   2  a null pointer still faults in a slot that came out of the
;      arena: the child is ended with 255 and its parent carries on
;   3  so does the last dword of the guard page, not just the first
;   4  every slot went back: the one slot in the arena afterwards is
;      this program's own, as it was before
;
; How deep 1 gets is the machine's business and is said in the report:
; seven levels below this one where memory allows (this is depth 1 and
; EXEC_MAX is 8), fewer on a 4MB 386.  Each level is this program
; again, told which it is in its tail - /L1, /L2 and on - and ends with
; the number of levels found below it, or 200 and up for a failure.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_ST     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_ST, DS:_ST, ES:_ST, SS:_ST
        ORG     0

BUF_LEN         EQU     10000h          ; each level's own memory to check

        D32_HEAD SLOT_IMAGE,<OFFSET st_entry>,<BUF_LEN>,4096

SLOT_BLK        EQU     PROG_SIZE + 0FF0h       ; what slot_new asks the
                                                ; arena for
BLK_USED        EQU     'M'                     ; F1h AL=8's signatures
BLK_END         EQU     'E'
CODE_FAIL       EQU     200                     ; a level that went wrong
CODE_LOST       EQU     201                     ; its memory changed under it
CODE_FAULT      EQU     255                     ; what a fault ends with

st_entry:
        ; The tail says which kind of run this is: /L<n> a level, /N a
        ; null store, /G a read of the guard's last dword, nothing at
        ; all the checks.
        mov     esi,SLOT_PSP + 81h
        movzx   ecx,byte ptr ds:[SLOT_PSP + 80h]
st_tail:
        cmp     ecx,2
        jb      st_checks
        cmp     byte ptr ds:[esi],'/'
        jne     st_tnext
        mov     al,ds:[esi+1]
        or      al,20h
        cmp     al,'n'
        je      st_null
        cmp     al,'g'
        je      st_guard
        cmp     al,'l'
        jne     st_tnext
        cmp     ecx,3
        jb      st_checks
        movzx   eax,byte ptr ds:[esi+2]
        sub     eax,'0'
        cmp     eax,9
        ja      st_checks
        jmp     st_level
st_tnext:
        inc     esi
        dec     ecx
        jmp     short st_tail

;--- the two that fault on purpose ------------------------------------
st_null:
        mov     dword ptr ds:[0],1      ; offset zero: the guard page
        mov     ax,4C00h                ; not reached, if it is there
        int     21h
st_guard:
        mov     eax,ds:[SLOT_PSP - 4]   ; and its last dword
        mov     ax,4C00h
        int     21h

;=====================================================================
; st_level - EAX = which level this is.  Fill this level's memory, start
;            the next one, and see that the memory is as it was left.
;            Ends with the levels found below, or CODE_FAIL/CODE_LOST.
;=====================================================================
st_level:
        mov     [level],eax
        call    fill

        mov     eax,[level]
        inc     eax
        call    run_level
        jnc     stl_ran
        cmp     ax,ERR_NOMEM            ; EXEC_MAX, or no arena left for a
        jne     stl_fail                ; slot: this is the bottom
        xor     ebx,ebx
        jmp     short stl_check
stl_ran:
        mov     ah,4Dh
        int     21h
        movzx   ebx,al
        cmp     ebx,CODE_FAIL
        jae     stl_out                 ; a failure below: pass it up
        inc     ebx                     ; the levels below, and that one
stl_check:
        call    check
        jc      stl_lost
stl_out:
        mov     eax,ebx
        mov     ah,4Ch
        int     21h
stl_fail:
        mov     ax,4C00h + CODE_FAIL
        int     21h
stl_lost:
        mov     ax,4C00h + CODE_LOST
        int     21h

;=====================================================================
; The checks - this program with an empty tail.
;=====================================================================
st_checks:
        mov     edx,OFFSET msg_hi
        call    puts
        call    count_slots
        mov     [slots0],eax

;--- 1: as deep as it goes --------------------------------------------
        mov     edx,OFFSET msg_t1
        call    puts
        mov     dword ptr [level],0
        call    fill
        mov     eax,1
        call    run_level
        jc      t1_bad
        mov     ah,4Dh
        int     21h
        movzx   eax,al
        cmp     eax,CODE_FAIL
        jae     t1_bad
        inc     eax                     ; the levels below level 1, and it
        mov     [deep],eax
        call    check
        jc      t1_bad
        call    good
        jmp     short t1_say
t1_bad:
        call    bad
t1_say:
        mov     edx,OFFSET msg_deep
        call    puts
        mov     eax,[deep]
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts

;--- 2 and 3: the guard page ------------------------------------------
        mov     edx,OFFSET msg_t2
        call    puts
        mov     edx,OFFSET tail_null
        call    run_fault
        mov     edx,OFFSET msg_t3
        call    puts
        mov     edx,OFFSET tail_guard
        call    run_fault

;--- 4: and the arena has them all back -------------------------------
        mov     edx,OFFSET msg_t4
        call    puts
        call    count_slots
        cmp     eax,[slots0]
        jne     t4_bad
        cmp     eax,1                   ; this one's, and no other
        jne     t4_bad
        call    good
        jmp     short wrapup
t4_bad:
        call    bad
        mov     edx,OFFSET msg_slots
        call    puts
        mov     eax,[slots0]
        call    putdec
        mov     edx,OFFSET msg_then
        call    puts
        call    count_slots
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts

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

;---------------------------------------------------------------------
; run_level - EAX = a level, 1 to 9.  This program as /L<level>.
;             CF and AX as 4Bh left them.
;---------------------------------------------------------------------
run_level:
        add     al,'0'
        mov     [tail_lvl + 4],al       ; length, blank, slash, L, digit
        mov     edx,OFFSET path_self
        mov     dword ptr [xblock + 4],OFFSET tail_lvl
        mov     ebx,OFFSET xblock
        mov     ax,4B00h
        int     21h
        ret

;---------------------------------------------------------------------
; run_fault - EDX -> a tail for a child that should fault.  It has to
;             start, and end with 255; ok or FAILED either way.
;---------------------------------------------------------------------
run_fault:
        mov     dword ptr [xblock + 4],edx
        mov     edx,OFFSET path_self
        mov     ebx,OFFSET xblock
        mov     ax,4B00h
        int     21h
        jc      rf_bad
        mov     ah,4Dh
        int     21h
        cmp     al,CODE_FAULT
        jne     rf_bad
        call    good
        ret
rf_bad:
        call    bad
        ret

;---------------------------------------------------------------------
; fill / check - this level's memory: every dword its own offset with
;                the level in the top byte.  check: CF if one is not.
;---------------------------------------------------------------------
fill:
        pushad
        mov     edi,OFFSET img_end
        mov     ecx,BUF_LEN / 4
        mov     eax,[level]
        shl     eax,24
        or      eax,edi
fl_lp:
        stosd
        add     eax,4
        loop    fl_lp
        popad
        ret

check:
        pushad
        mov     esi,OFFSET img_end
        mov     ecx,BUF_LEN / 4
        mov     eax,[level]
        shl     eax,24
        or      eax,esi
ck_lp:
        cmp     [esi],eax
        jne     ck_bad
        add     esi,4
        add     eax,4
        loop    ck_lp
        popad
        clc
        ret
ck_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; count_slots - EAX = the kernel's blocks that are a slot: in use, owned
;               by nobody (slot_new buys them with owner 0) and the
;               size slot_new asks for, give or take a remainder too
;               small to split off.
;---------------------------------------------------------------------
count_slots:
        push    ebx
        push    edx
        push    esi
        push    edi
        xor     esi,esi
        xor     ebx,ebx
cs_lp:
        mov     edx,F8_ARENA
        mov     edi,OFFSET blk
        mov     ax,0F108h
        int     21h
        jc      cs_out
        cmp     dword ptr [blk + F8_SIG],BLK_END
        je      cs_out
        cmp     dword ptr [blk + F8_SIG],BLK_USED
        jne     cs_next
        cmp     dword ptr [blk + F8_OWNER],0
        jne     cs_next
        mov     eax,dword ptr [blk + F8_SIZE]
        sub     eax,SLOT_BLK
        cmp     eax,32
        jae     cs_next
        inc     esi
cs_next:
        inc     ebx
        cmp     ebx,100000
        jb      cs_lp
cs_out:
        mov     eax,esi
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        ret

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
msg_hi      db  'SLOTTEST - every EXEC level in a slot of its own',13,10,13,10,'$'
msg_t1      db  ' 1 levels nest, and keep their memory    ','$'
msg_deep    db  '     levels below this one: ','$'
msg_t2      db  ' 2 a null pointer still faults: 255      ','$'
msg_t3      db  " 3 ...and the guard page's last dword    ",'$'
msg_t4      db  ' 4 every slot went back to the arena     ','$'
msg_slots   db  '     slots in the arena before: ','$'
msg_then    db  ', after: ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'SLOTTEST: all checks passed',13,10,'$'
msg_fail    db  'SLOTTEST: FAILURES',13,10,'$'

path_self   db  'C:\TESTS\SLOTTEST.EXE',0
xblock      dd  0                       ; no environment
            dd  0                       ; and the tail, filled in
tail_lvl    db  4,' /L0',0Dh
tail_null   db  3,' /N',0Dh
tail_guard  db  3,' /G',0Dh

fails       db  0
            db  0,0,0
level       dd  0
deep        dd  0
slots0      dd  0
blk         db  F8_LEN dup(0)
resptr      dd  OFFSET results
results     db  16 dup('$')

img_end     LABEL BYTE                  ; the BSS: BUF_LEN of this level's

_ST     ENDS
        END
