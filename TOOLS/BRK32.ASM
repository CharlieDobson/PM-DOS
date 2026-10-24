;=====================================================================
; BRK32.ASM - BRK32.EXE: BREAK for a native program.
;
; BRKTEST.COM checks the rules from a DOS box.  A native program is
; reached by a different door (int21_entry, not k21) and its INT 23h by
; a different road (r3_thunk, at ring 3, coming back through the
; trampoline at PSP+54h), and a ^C there carries on where it stopped
; rather than making the call again.  So the rules are checked again
; from here:
;
;   BREAK OFF: 19h leaves a waiting ^C; 0Bh and 64 characters through
;     02h take it
;   BREAK ON: 19h takes it, and a ^C behind a key waits for the key
;   0Bh takes ^S with the key after it, and ^P and Ctrl+PrtSc - each
;     twice, so that the printer echo ends as it began; and 06h reads
;     Ctrl+PrtSc as ^P, which says the keyboard made the key at all
;
; A native program cannot look at the BIOS's buffer, so keys are
; pressed with 21h/F1h AL=23h, scan codes as the keyboard sends them,
; and what is left is read back with 06h, which takes a ^C as a key.
; Each case prints only when nothing is waiting, as BRKTEST's do.
;
; The exit code is the number of failures.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_B3     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_B3, DS:_B3, ES:_B3, SS:_B3
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET b3_entry>,<0>,4096

b3_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        mov     ax,3300h                ; BREAK as it was, to put back
        int     21h
        mov     [was_brk],dl

        mov     ax,3523h
        int     21h
        mov     [old23],ebx
        mov     edx,OFFSET my23
        mov     ax,2523h
        int     21h

        call    t_offdisk
        call    t_ondisk
        call    t_behind
        call    t_off0b
        call    t_off02
        call    t_stop
        call    t_prtraw
        mov     esi,OFFSET sc_cp
        mov     edx,OFFSET m_ctlp
        call    t_ctlp
        mov     esi,OFFSET sc_prtsc
        mov     edx,OFFSET m_prtsc
        call    t_ctlp

        call    kb_flush                ; nothing left for the shell
        mov     edx,[old23]
        mov     ax,2523h
        int     21h
        mov     dl,[was_brk]
        mov     ax,3301h
        int     21h

        mov     edx,OFFSET msg_end
        call    puts
        movzx   eax,byte ptr [nfail]
        call    putdec
        mov     edx,OFFSET msg_failed
        call    puts
        mov     al,[nfail]
        mov     ah,4Ch
        int     21h

;=====================================================================
; The cases
;=====================================================================

;--- BREAK OFF: 19h leaves it ------------------------------------------
t_offdisk:
        mov     dl,0
        call    arm_cc
        mov     ah,19h
        int     21h
        call    take_key
        mov     [r1],eax
        call    kb_flush
        mov     edx,OFFSET m_offdisk
        cmp     dword ptr [ncc],0
        jne     fail
        cmp     dword ptr [r1],3        ; still there for 06h to read
        jne     fail
        jmp     pass

;--- BREAK ON: 19h takes it, and carries on ----------------------------
t_ondisk:
        mov     dl,1
        call    arm_cc
        mov     ah,19h
        int     21h
        mov     [r2],al                 ; the call's own answer, after
        call    take_key
        mov     [r1],eax
        call    kb_flush
        mov     edx,OFFSET m_ondisk
        cmp     dword ptr [ncc],1
        jne     fail
        cmp     dword ptr [r1],-1
        jne     fail
        cmp     byte ptr [r2],25        ; a drive, A: to Z:
        ja      fail
        jmp     pass

;--- BREAK ON: a ^C behind a key waits for it --------------------------
t_behind:
        mov     dl,1
        call    set_brk
        call    kb_flush
        mov     dword ptr [ncc],0
        mov     esi,OFFSET sc_a
        call    press
        mov     esi,OFFSET sc_cc
        call    press
        mov     ah,19h
        int     21h
        mov     eax,[ncc]
        mov     [r3],eax                ; no ^C yet
        call    take_key                ; the A, read
        mov     [r1],eax
        mov     ah,19h                  ; and now the ^C is in front
        int     21h
        call    take_key
        mov     [r2],al
        mov     [r4],eax
        call    kb_flush
        mov     edx,OFFSET m_behind
        cmp     dword ptr [r3],0
        jne     fail
        cmp     dword ptr [r1],'a'      ; no Shift: a small one
        jne     fail
        cmp     dword ptr [ncc],1
        jne     fail
        cmp     dword ptr [r4],-1
        jne     fail
        jmp     pass

;--- BREAK OFF: 0Bh takes it -------------------------------------------
t_off0b:
        mov     dl,0
        call    arm_cc
        mov     ah,0Bh
        int     21h
        mov     [r2],al
        call    take_key
        mov     [r1],eax
        call    kb_flush
        mov     edx,OFFSET m_off0b
        cmp     dword ptr [ncc],1
        jne     fail
        cmp     dword ptr [r1],-1
        jne     fail
        cmp     byte ptr [r2],0         ; and then nothing is waiting
        jne     fail
        jmp     pass

;--- BREAK OFF: 64 characters through 02h take it ----------------------
t_off02:
        mov     dl,0
        call    arm_cc
        mov     ecx,64
t02_lp:
        mov     dl,'.'
        mov     ah,02h
        int     21h
        loop    t02_lp
        call    take_key
        mov     [r1],eax
        call    kb_flush
        call    crlf
        mov     edx,OFFSET m_off02
        cmp     dword ptr [ncc],1
        jne     fail
        cmp     dword ptr [r1],-1
        jne     fail
        jmp     pass

;--- ^S at 0Bh: taken, with the key that ends the wait -----------------
t_stop:
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     dword ptr [ncc],0
        mov     esi,OFFSET sc_cs
        call    press
        mov     esi,OFFSET sc_a         ; ends the wait, and goes with it
        call    press
        mov     esi,OFFSET sc_x         ; stays
        call    press
        mov     ah,0Bh
        int     21h
        mov     [r2],al
        call    take_key
        mov     [r1],eax
        call    kb_flush
        mov     edx,OFFSET m_stop
        cmp     byte ptr [r2],0FFh
        jne     fail
        cmp     dword ptr [r1],'x'
        jne     fail
        cmp     dword ptr [ncc],0
        jne     fail
        jmp     pass

;--- Ctrl+PrtSc is a key, and 06h reads it as ^P -----------------------
; Without this the case below would pass on a keyboard that made
; nothing of the key at all.
t_prtraw:
        call    kb_flush
        mov     esi,OFFSET sc_prtsc
        call    press
        call    take_key
        mov     [r1],eax
        call    kb_flush
        mov     edx,OFFSET m_prtraw
        cmp     dword ptr [r1],'P' - '@'
        jne     fail
        jmp     pass

;--- ESI -> ^P's or Ctrl+PrtSc's scan codes, EDX -> the case's name:
;--- 0Bh takes the key, twice --------------------------------------------
t_ctlp:
        mov     [keyp],esi
        mov     [msgp],edx
        mov     dl,0
        call    set_brk
        call    kb_flush
        mov     esi,[keyp]
        call    press
        mov     esi,OFFSET sc_x
        call    press
        mov     ah,0Bh                  ; the echo on
        int     21h
        mov     [r2],al
        call    take_key
        mov     [r1],eax
        call    kb_flush
        mov     esi,[keyp]
        call    press
        mov     ah,0Bh                  ; and off
        int     21h
        mov     byte ptr [r3],al
        call    take_key
        mov     [r4],eax
        call    kb_flush
        mov     edx,[msgp]
        cmp     byte ptr [r2],0FFh      ; the x after it still waits
        jne     fail
        cmp     dword ptr [r1],'x'
        jne     fail
        cmp     byte ptr [r3],0
        jne     fail
        cmp     dword ptr [r4],-1
        jne     fail
        jmp     pass

;=====================================================================
; The handler, at ring 3: count, and carry on
;=====================================================================
my23:
        inc     dword ptr [ncc]
        iretd

;=====================================================================
; Helpers
;=====================================================================
set_brk:
        mov     ax,3301h
        int     21h
        ret

; arm_cc - DL = BREAK; the keyboard emptied, the count zeroed, and one
;          ^C pressed
arm_cc:
        call    set_brk
        call    kb_flush
        mov     dword ptr [ncc],0
        mov     esi,OFFSET sc_cc
        jmp     press

; take_key - EAX = the next key's character, taken with 06h, or -1
take_key:
        mov     dl,0FFh
        mov     ah,06h
        int     21h
        jz      tk_none
        movzx   eax,al
        ret
tk_none:
        mov     eax,-1
        ret

kb_flush:
        call    take_key
        cmp     eax,-1
        jne     kb_flush
        ret

; press - ESI -> scan codes, ending in a zero, through 21h/F1h AL=23h
press:
        lodsb
        or      al,al
        jz      pr_out
        mov     dl,al
        mov     ax,0F123h
        int     21h
        jmp     short press
pr_out:
        ret

pass:
        push    edx
        mov     edx,OFFSET m_ok
        call    puts
        pop     edx
        call    puts
        ret
fail:
        push    edx
        mov     edx,OFFSET m_fail
        call    puts
        pop     edx
        call    puts
        inc     byte ptr [nfail]
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    puts
        pop     edx
        ret

; putdec - EAX in decimal
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
ncc         dd  0                       ; INT 23h calls
r1          dd  0
r2          db  0
            db  0,0,0
r3          dd  0
r4          dd  0
old23       dd  0
keyp        dd  0
msgp        dd  0
nfail       db  0
was_brk     db  0
numbuf      db  12 dup(0)

sc_cc       db  1Dh, 2Eh, 0AEh, 9Dh, 0  ; Ctrl down, C, Ctrl up
sc_cs       db  1Dh, 1Fh, 9Fh, 9Dh, 0   ; Ctrl+S
sc_cp       db  1Dh, 19h, 99h, 9Dh, 0   ; Ctrl+P
sc_prtsc    db  1Dh, 0E0h, 37h, 0E0h, 0B7h, 9Dh, 0  ; Ctrl and Print Screen
sc_a        db  1Eh, 9Eh, 0
sc_x        db  2Dh, 0ADh, 0

msg_hi      db  'BRK32: BREAK, for a native program',13,10,'$'
msg_end     db  'BRK32: $'
msg_failed  db  ' failed',13,10,'$'
msg_crlf    db  13,10,'$'
m_ok        db  '  ok    $'
m_fail      db  '  FAIL  $'

m_offdisk   db  'BREAK OFF: 19h leaves a waiting ^C where it is',13,10,'$'
m_ondisk    db  'BREAK ON: 19h takes it, and the call is finished',13,10,'$'
m_behind    db  'BREAK ON: a ^C behind a key waits until the key is read',13,10,'$'
m_off0b     db  'BREAK OFF: 0Bh takes it',13,10,'$'
m_off02     db  'BREAK OFF: 64 characters through 02h take a ^C',13,10,'$'
m_stop      db  '^S at 0Bh: taken, with the key that ends the wait',13,10,'$'
m_prtraw    db  '06h reads Ctrl+PrtSc as ^P',13,10,'$'
m_ctlp      db  '^P at 0Bh is taken, for the printer echo',13,10,'$'
m_prtsc     db  'Ctrl+PrtSc is ^P',13,10,'$'

_B3     ENDS
        END
