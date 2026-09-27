;=====================================================================
; FNDTEST.ASM - FNDTEST.EXE, the two finds and what they say when they
;               cannot find anything.
;
; Two kernel changes of v0.60 are tested here and neither shows in a
; listing that works:
;
;   4Eh AND 714Eh NOW SAY "PATH NOT FOUND" (3) WHEN THE DIRECTORY IS
;   WHAT IS MISSING.  Every failure used to come back 2, file not
;   found, because f4E_none wrote ERR_NOFILE over whatever the path
;   resolver had said - so a program could not tell "nothing matched"
;   from "that directory does not exist".  ATTRIB had to ask about the
;   directory separately to print DOS's two different sentences.
;
;   THE LONG-NAME FIND HAS 32 HANDLES AND A PROGRAM'S ARE TAKEN BACK
;   WHEN IT ENDS.  There were four, in the image, owned by nobody: a
;   program killed in the middle of a directory walk kept its handle
;   until the reboot, and ATTRIB /S could not go five levels deep.
;
; Run as FNDTEST it makes the ten checks below.  Run as FNDTEST LEAK it
; opens every handle there is and exits WITHOUT closing one, which is
; the set-up for the sweep: FNDTEST run straight afterwards can only
; pass check 7 if 4Ch gave them all back.  FNDTEST.BAT does the two in
; that order.
;
;   1  4Eh on a missing directory is 3
;   2  4Eh on nothing matching in a real one is 2 (or 12h)
;   3  4Eh on a drive that is not there is 0Fh
;   4  714Eh on a missing directory is 3
;   5  714Eh on nothing matching is 2
;   6  714Eh on a missing drive is 0Fh
;   7  32 searches open at once, every handle different
;   8  ...and a 33rd is refused with 4, too many open
;   9  71A1h closes every one of them
;  10  a handle that was closed is refused by 714Fh with 6
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_FT     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_FT, DS:_FT, ES:_FT, SS:_FT
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET ft_entry>,<0>,4096

NHANDLE         EQU     32              ; = FIND_MAX, which is what is tested
FRECLEN         EQU     318             ; a 714Eh record - NOT "FREC": wasm
                                        ; folds case and frec is the buffer

ft_entry:
        mov     edx,OFFSET dta
        mov     ah,1Ah
        int     21h

        ; "LEAK" anywhere in the tail: take everything and go
        mov     esi,SLOT_PSP + 81h
        movzx   ecx,byte ptr ds:[SLOT_PSP + 80h]
ft_tail:
        or      ecx,ecx
        jz      ft_checks
        mov     al,ds:[esi]
        or      al,20h
        cmp     al,'l'
        jne     ft_tnext
        cmp     ecx,4
        jb      ft_checks
        mov     eax,ds:[esi]
        or      eax,20202020h
        cmp     eax,'kael'              ; "leak", little-endian
        je      ft_leak
ft_tnext:
        inc     esi
        dec     ecx
        jmp     short ft_tail

ft_leak:
        call    open_all                ; ...and do not close them
        mov     edx,OFFSET msg_leak
        call    puts
        mov     eax,[nopen]
        call    putdec
        mov     edx,OFFSET msg_leak2
        call    puts
        mov     ax,4C00h
        int     21h

ft_checks:
        mov     edx,OFFSET msg_hi
        call    puts

;--- 1 - 3: the classic find ------------------------------------------
        mov     edx,OFFSET msg_t1
        mov     esi,OFFSET pat_nodir
        mov     bx,ERR_NOPATH
        call    try4E
        mov     edx,OFFSET msg_t2
        mov     esi,OFFSET pat_nomatch
        mov     bx,ERR_NOFILE
        call    try4E
        mov     edx,OFFSET msg_t3
        mov     esi,OFFSET pat_nodrv
        mov     bx,ERR_BADDRIVE
        call    try4E

;--- 4 - 6: the long-name find ----------------------------------------
        mov     edx,OFFSET msg_t4
        mov     esi,OFFSET pat_nodir
        mov     bx,ERR_NOPATH
        call    try714E
        mov     edx,OFFSET msg_t5
        mov     esi,OFFSET pat_nomatch
        mov     bx,ERR_NOFILE
        call    try714E
        mov     edx,OFFSET msg_t6
        mov     esi,OFFSET pat_nodrv
        mov     bx,ERR_BADDRIVE
        call    try714E

;--- 7: thirty-two at once, every one different -----------------------
        mov     edx,OFFSET msg_t7
        call    puts
        call    open_all
        cmp     dword ptr [nopen],NHANDLE
        jne     t7_bad
        xor     ebx,ebx                 ; no two alike
t7_outer:
        cmp     ebx,NHANDLE
        jae     t7_good
        mov     ecx,ebx
        inc     ecx
t7_inner:
        cmp     ecx,NHANDLE
        jae     t7_onext
        mov     ax,[handles+ebx*2]
        cmp     ax,[handles+ecx*2]
        je      t7_bad
        inc     ecx
        jmp     short t7_inner
t7_onext:
        inc     ebx
        jmp     short t7_outer
t7_good:
        call    good
        jmp     short t8
t7_bad:
        call    bad
        mov     edx,OFFSET msg_only
        call    puts
        mov     eax,[nopen]
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts
t8:

;--- 8: and there is no thirty-third ----------------------------------
        mov     edx,OFFSET msg_t8
        call    puts
        mov     edx,OFFSET pat_all
        mov     edi,OFFSET frec
        mov     ecx,ATTR_DIR
        mov     esi,1
        mov     ax,714Eh
        int     21h
        jnc     t8_bad
        cmp     ax,ERR_NOHANDLES
        jne     t8_bad
        call    good
        jmp     short t9
t8_bad:
        call    bad
t9:

;--- 9: 71A1h closes every one ----------------------------------------
        mov     edx,OFFSET msg_t9
        call    puts
        mov     byte ptr [closebad],0
        xor     ecx,ecx
t9_lp:
        cmp     ecx,[nopen]
        jae     t9_done
        push    ecx
        movzx   ebx,word ptr [handles+ecx*2]
        mov     ax,71A1h
        int     21h
        pop     ecx
        jnc     t9_next
        mov     byte ptr [closebad],1
t9_next:
        inc     ecx
        jmp     short t9_lp
t9_done:
        cmp     byte ptr [closebad],0
        jne     t9_bad
        cmp     dword ptr [nopen],0
        je      t9_bad
        call    good
        jmp     short t10
t9_bad:
        call    bad
t10:

;--- 10: a closed handle is not a handle ------------------------------
        mov     edx,OFFSET msg_t10
        call    puts
        movzx   ebx,word ptr [handles]
        mov     edi,OFFSET frec
        mov     esi,1
        mov     ax,714Fh
        int     21h
        jnc     t10_bad
        cmp     ax,ERR_BADHANDLE
        jne     t10_bad
        call    good
        jmp     short wrapup
t10_bad:
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
; try4E / try714E - EDX = the check's title, ESI = the pattern, BX = the
;                   error it must fail with.  4Eh's "no match" may also
;                   honestly be 12h, no more files, which some DOSes say
;                   on a first call; the path and drive errors may not.
;---------------------------------------------------------------------
try4E:
        call    puts
        mov     edx,esi
        mov     ecx,ATTR_DIR
        mov     ah,4Eh
        int     21h
        jmp     short try_judge
try714E:
        call    puts
        mov     edx,esi
        mov     edi,OFFSET frec
        mov     ecx,ATTR_DIR
        mov     esi,1
        mov     ax,714Eh
        int     21h
        jnc     try_judge
        cmp     ax,7100h                ; LFN=OFF: nothing to test
        jne     try_judge
        stc
try_judge:
        jnc     tj_bad                  ; it found something
        cmp     ax,bx
        je      tj_good
        cmp     bx,ERR_NOFILE
        jne     tj_bad
        cmp     ax,12h
        je      tj_good
tj_bad:
        mov     [got],ax
        call    bad
        mov     edx,OFFSET msg_got
        call    puts
        movzx   eax,word ptr [got]
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts
        ret
tj_good:
        call    good
        ret

;---------------------------------------------------------------------
; open_all - as many 714Eh searches of the root as the kernel will give,
;            up to NHANDLE, handles into [handles], the count in [nopen].
;---------------------------------------------------------------------
open_all:
        mov     dword ptr [nopen],0
oa_lp:
        cmp     dword ptr [nopen],NHANDLE
        jae     oa_out
        mov     edx,OFFSET pat_all
        mov     edi,OFFSET frec
        mov     ecx,ATTR_DIR
        mov     esi,1
        mov     ax,714Eh
        int     21h
        jc      oa_out
        mov     ecx,[nopen]
        mov     [handles+ecx*2],ax
        inc     dword ptr [nopen]
        jmp     short oa_lp
oa_out:
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
msg_hi      db  'FNDTEST - what the finds say when there is nothing',13,10,13,10,'$'
msg_t1      db  ' 1 4Eh, no such directory: 3             ','$'
msg_t2      db  ' 2 4Eh, nothing matches: 2               ','$'
msg_t3      db  ' 3 4Eh, no such drive: 0Fh               ','$'
msg_t4      db  ' 4 714Eh, no such directory: 3           ','$'
msg_t5      db  ' 5 714Eh, nothing matches: 2             ','$'
msg_t6      db  ' 6 714Eh, no such drive: 0Fh             ','$'
msg_t7      db  ' 7 32 searches open at once              ','$'
msg_t8      db  ' 8 ...and no 33rd: 4                     ','$'
msg_t9      db  ' 9 71A1h closes them all                 ','$'
msg_t10     db  '10 a closed handle is refused: 6         ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_got     db  '     it said: ','$'
msg_only    db  '     searches the kernel gave: ','$'
msg_leak    db  'FNDTEST LEAK: left ','$'
msg_leak2   db  ' searches open on purpose',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'FNDTEST: all checks passed',13,10,'$'
msg_fail    db  'FNDTEST: FAILURES',13,10,'$'

pat_nodir   db  '\NOSUCH.DIR\*.*',0
pat_nomatch db  '\*.QQQ',0
pat_nodrv   db  'Q:\*.*',0
pat_all     db  '\*.*',0

fails       db  0
closebad    db  0
got         dw  0
nopen       dd  0
resptr      dd  OFFSET results
results     db  16 dup('$')
handles     dw  NHANDLE dup(0)
dta         db  128 dup(0)
frec        db  FRECLEN dup(0)

_FT     ENDS
        END
