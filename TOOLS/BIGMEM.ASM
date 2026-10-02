;=====================================================================
; BIGMEM.ASM - BIGMEM.EXE, a native program with more than 512K
;
; Until v0.62 a native program's memory was its slot and its slot was
; 512K, whatever the machine had.  48h now grows the slot when the heap
; is short (RING3.INC, slot_grow), so a program gets memory the way a
; C program's malloc would ask for it: one 48h at a time, with nothing
; in its header and no call of its own to make.  This is that program:
;
;   1  48h for FFFFFFFFh is refused, and the largest it says it could
;      have is more than the 512K slot it started in
;   2  that much comes as ONE block - every page of it written and read
;      back, and its first and last 64K filled and checked in full
;   3  freed, it comes back as sixteen blocks, each touched the same way
;   4  holding all but 2MB of it, a child that has to grow its own slot
;      still runs (on a machine with more than 4MB to give)
;   5  the PSP's memory top says the slot is as big as it now is
;
; The size in 1 is the machine's and is reported; everything else is
; ok or FAILED.  The child is this program again, as /C: it asks 48h
; for 1MB, which is more than its slot starts with, and ends with 42.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_BM     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_BM, DS:_BM, ES:_BM, SS:_BM
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET bm_entry>,<0>,4096

CHILD_CODE      EQU     42
CHILD_ASK       EQU     00100000h       ; what the child asks for: 1MB
PIECES          EQU     16
ENDS_LEN        EQU     10000h          ; filled in full at each end
KEEP_FREE       EQU     00200000h       ; what 4 leaves for the child

bm_entry:
        ; "/C" in the tail: the child
        mov     esi,SLOT_PSP + 81h
        movzx   ecx,byte ptr ds:[SLOT_PSP + 80h]
bm_tail:
        cmp     ecx,2
        jb      bm_checks
        cmp     byte ptr ds:[esi],'/'
        jne     bm_tnext
        mov     al,ds:[esi+1]
        or      al,20h
        cmp     al,'c'
        je      bm_child
bm_tnext:
        inc     esi
        dec     ecx
        jmp     short bm_tail

bm_child:
        mov     ebx,CHILD_ASK
        mov     ah,48h
        int     21h
        jc      bmc_bad
        mov     edi,eax
        mov     ecx,CHILD_ASK
        call    touch
        jc      bmc_bad
        mov     ax,4C00h + CHILD_CODE
        int     21h
bmc_bad:
        mov     ax,4C01h
        int     21h

;=====================================================================
bm_checks:
        mov     edx,OFFSET msg_hi
        call    puts

;--- 1: how much there is --------------------------------------------
        mov     edx,OFFSET msg_t1
        call    puts
        call    largest
        mov     [most],eax
        cmp     eax,PROG_SIZE
        jbe     t1_bad
        call    good
        jmp     short t1_say
t1_bad:
        call    bad
t1_say:
        mov     edx,OFFSET msg_most
        call    puts
        mov     eax,[most]
        shr     eax,10
        call    putdec
        mov     edx,OFFSET msg_kb
        call    puts

;--- 2: all of it, in one block --------------------------------------
        mov     edx,OFFSET msg_t2
        call    puts
        mov     ebx,[most]
        mov     ah,48h
        int     21h
        jc      t2_bad
        mov     [blk],eax
        mov     edi,eax
        mov     ecx,[most]
        call    touch
        jc      t2_bad
        call    good
        jmp     short t3
t2_bad:
        call    bad

;--- 3: given back, and taken again in pieces ------------------------
t3:
        mov     edx,OFFSET msg_t3
        call    puts
        mov     edx,[blk]
        or      edx,edx
        jz      t3_bad
        mov     ah,49h
        int     21h
        jc      t3_bad
        mov     dword ptr [blk],0
        mov     eax,[most]
        xor     edx,edx
        mov     ecx,PIECES
        div     ecx
        sub     eax,64                  ; a header each, and some
        and     eax,NOT 0FFFh
        mov     [piece],eax
        xor     esi,esi
t3_take:
        mov     ebx,[piece]
        mov     ah,48h
        int     21h
        jc      t3_bad
        mov     [piece_at+esi*4],eax
        mov     edi,eax
        mov     ecx,[piece]
        call    touch
        jc      t3_bad
        inc     esi
        cmp     esi,PIECES
        jb      t3_take
t3_give:
        dec     esi
        mov     edx,[piece_at+esi*4]
        mov     ah,49h
        int     21h
        jc      t3_bad
        or      esi,esi
        jnz     t3_give
        call    good
        jmp     short t4
t3_bad:
        call    bad

;--- 4: a child, with this program holding nearly everything ---------
t4:
        mov     edx,OFFSET msg_t4
        call    puts
        call    largest
        cmp     eax,KEEP_FREE * 2
        jae     t4_room
        mov     dl,'-'                  ; not enough machine to say
        call    tally
        mov     edx,OFFSET msg_skip
        call    puts
        jmp     short t5
t4_room:
        sub     eax,KEEP_FREE
        mov     ebx,eax
        mov     ah,48h
        int     21h
        jc      t4_bad
        mov     [blk],eax
        mov     edx,OFFSET path_self
        mov     ebx,OFFSET xblock
        mov     ax,4B00h
        int     21h
        jc      t4_bad
        mov     ah,4Dh
        int     21h
        cmp     al,CHILD_CODE
        jne     t4_bad
        mov     edx,[blk]
        mov     ah,49h
        int     21h
        jc      t4_bad
        call    good
        jmp     short t5
t4_bad:
        call    bad

;--- 5: the PSP knows ------------------------------------------------
t5:
        mov     edx,OFFSET msg_t5
        call    puts
        mov     eax,ds:[SLOT_PSP + PSP_MEMTOP]
        cmp     eax,PROG_SIZE
        jbe     t5_bad
        test    eax,0FFFh               ; whole pages
        jnz     t5_bad
        mov     al,ds:[eax-1]           ; and the last byte is there to read
        call    good
        jmp     short wrapup
t5_bad:
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

;---------------------------------------------------------------------
; largest - EAX = what 48h says the largest block it could give is.
;---------------------------------------------------------------------
largest:
        push    ebx
        mov     ebx,0FFFFFFFFh
        mov     ah,48h
        int     21h
        mov     eax,ebx
        jc      lg_out                  ; refused, as it must be
        xor     eax,eax                 ; ...and if it was not, nothing is
lg_out:                                 ; known
        pop     ebx
        ret

;---------------------------------------------------------------------
; touch - EDI = a block, ECX = its length.  Every page gets its own
;         offset, read back after all are written; the first and last
;         ENDS_LEN (or the whole of a smaller block) get a pattern in
;         every dword.  CF if anything did not come back.
;---------------------------------------------------------------------
touch:
        pushad
        mov     ebx,edi
        lea     edx,[edi+ecx]           ; EDX = one past the end
        mov     esi,edi
tc_wr:
        mov     [esi],esi
        add     esi,1000h
        cmp     esi,edx
        jb      tc_wr
        mov     esi,ebx
tc_rd:
        cmp     [esi],esi
        jne     tc_bad
        add     esi,1000h
        cmp     esi,edx
        jb      tc_rd

        mov     eax,ENDS_LEN            ; the front
        cmp     eax,ecx
        jbe     tc_front
        mov     eax,ecx
tc_front:
        and     eax,NOT 3
        mov     edi,ebx
        call    fillchk
        jc      tc_bad
        mov     edi,edx                 ; and the back
        sub     edi,eax
        and     edi,NOT 3
        cmp     edi,ebx
        jae     tc_back
        mov     edi,ebx
tc_back:
        call    fillchk
        jc      tc_bad
        popad
        clc
        ret
tc_bad:
        popad
        stc
        ret

; fillchk - EDI = where, EAX = bytes (a multiple of 4).  Each dword is
; its own address, inverted, written all and then read all.
fillchk:
        pushad
        mov     ecx,eax
        shr     ecx,2
        jz      fc_ok
        mov     esi,edi
        mov     edx,ecx
fc_wr:
        mov     eax,edi
        not     eax
        stosd
        loop    fc_wr
        mov     ecx,edx
fc_rd:
        mov     eax,esi
        not     eax
        cmp     [esi],eax
        jne     fc_bad
        add     esi,4
        loop    fc_rd
fc_ok:
        popad
        clc
        ret
fc_bad:
        popad
        stc
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
msg_hi      db  'BIGMEM - a native program with more than 512K',13,10,13,10,'$'
msg_t1      db  ' 1 48h offers more than the slot it had  ','$'
msg_most    db  '     largest it could give: ','$'
msg_kb      db  'K',13,10,'$'
msg_t2      db  ' 2 all of it in one block, every page    ','$'
msg_t3      db  ' 3 given back, and again in 16 pieces    ','$'
msg_t4      db  ' 4 a child grows while this holds it all ','$'
msg_t5      db  ' 5 the PSP',39,'s memory top has grown       ','$'
msg_skip    db  'skipped',13,10,'$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'BIGMEM: all checks passed',13,10,'$'
msg_fail    db  'BIGMEM: FAILURES',13,10,'$'

path_self   db  'C:\TESTS\BIGMEM.EXE',0
xblock      dd  0                       ; no environment
            dd  OFFSET child_tail
child_tail  db  3,' /C',0Dh

fails       db  0
            db  0,0,0
most        dd  0
blk         dd  0
piece       dd  0
piece_at    dd  PIECES dup(0)
resptr      dd  OFFSET results
results     db  16 dup('$')

_BM     ENDS
        END
