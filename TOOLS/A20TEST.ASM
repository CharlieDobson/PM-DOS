;=====================================================================
; A20TEST.ASM - A20TEST.COM, a 16-bit DOS program that goes at the A20
;               gate the way a program that has never heard of XMS
;               goes at it: through the hardware.
;
; XMSTEST already proves the gate works when it is asked politely, by
; a driver function written for the purpose.  This one asks nobody.
; It writes port 92h, it talks to the 8042, and it calls the BIOS -
; the three routes every real A20 routine has ever used - and after
; each one it looks at the 1MB wrap to see whether anything actually
; happened.
;
; THE POINT IS NOT THAT THE GATE MOVES.  It is that the gate the
; program moves and the gate the kernel believes in are the SAME gate.
; PM-DOS emulates A20 in the page tables, so a guest allowed to reach
; the real hardware could put the two into disagreement - and worse,
; clearing the real line would take bit 20 away from protected-mode
; addresses too and unmap the kernel from underneath itself.  So the
; monitor claims ports 60h, 64h and 92h in the TSS I/O bitmap and
; emulates the eight IN and OUT opcodes for them.  Every check below
; is a check on that interception.
;
; The wrap probe writes 0000:0500 and FFFF:0510, which are the same
; byte on an 8086 and different bytes on anything with A20 open.
; 0500h rather than 0000h, because scribbling on INT 0 to prove a
; point takes the machine with it.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_A20    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_A20, DS:_A20, ES:_A20, SS:_A20
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- where the gate starts -------------------------------------------
        call    probe
        mov     byte ptr [entry_a20],al
        mov     dx,OFFSET msg_start
        mov     ah,09h
        int     21h
        call    say_state

;=====================================================================
; Route one: the PS/2 system control port.
;=====================================================================
        mov     dx,OFFSET msg_r92
        mov     ah,09h
        int     21h

        mov     al,1
        call    set92
        mov     dx,OFFSET msg_92on
        mov     ah,1                    ; expect: wrap gone
        call    check

        mov     al,0
        call    set92
        mov     dx,OFFSET msg_92off
        mov     ah,0                    ; expect: wrap back
        call    check

;--- and it has to read back what it was told ------------------------
        mov     al,1
        call    set92
        in      al,92h
        test    al,02h
        jz      r92_bad
        mov     dx,OFFSET msg_92rd
        call    say_ok
        jmp     short did_r92
r92_bad:
        mov     dx,OFFSET msg_92rdbad
        call    say_fail
did_r92:

;=====================================================================
; Route two: the keyboard controller, the long way round.  Command
; D1h says "the next byte on 60h is your output port", and bit 1 of
; that byte is A20.  This is what HIMEM.SYS does.
;=====================================================================
        mov     dx,OFFSET msg_rkbc
        mov     ah,09h
        int     21h

        mov     al,0
        call    setkbc
        mov     dx,OFFSET msg_kbcoff
        mov     ah,0
        call    check

        mov     al,1
        call    setkbc
        mov     dx,OFFSET msg_kbcon
        mov     ah,1
        call    check

;--- D0h, the other direction: read the output port back -------------
; The interesting part is that it terminates.  The real 8042 never saw
; the D0h, so if the monitor did not fake the "byte ready" status bit
; this loop would spin until it gave up.
        call    kbcwait
        mov     al,0D0h
        out     64h,al
        mov     cx,0FFFFh
d0_wait:
        in      al,64h
        test    al,01h
        jnz     d0_ready
        loop    d0_wait
        mov     dx,OFFSET msg_d0hang
        call    say_fail
        jmp     short did_d0
d0_ready:
        in      al,60h
        test    al,02h                  ; A20 is on, so bit 1 must be set
        jz      d0_bad
        mov     dx,OFFSET msg_d0ok
        call    say_ok
        jmp     short did_d0
d0_bad:
        mov     dx,OFFSET msg_d0bad
        call    say_fail
did_d0:

;--- and D0h again, with the gate the other way ----------------------
; Not a repeat for the sake of it.  The monitor has a little state
; machine behind these two commands - a flag saying "a D0h is
; outstanding, the next read of 60h is mine" and another for D1h - and
; a flag that failed to clear would show up here and nowhere else:
; the first round trip would work and every one after it would return
; a stale answer or swallow a real scancode.
;
; What is NOT tested, because it cannot be: whether the physical 8042
; is still in a sane state.  Asking it - 20h, "give me your command
; byte" - does get an answer on real hardware, and PM-DOS's own IRQ 1
; handler reads it off port 60h before the guest's polling loop can
; get near it, and types it at the shell as a scancode.  The host owns
; the keyboard; a DOS box cannot hold a two-way conversation with the
; controller behind its back.  See NOTES.TXT.
        mov     al,0
        call    setkbc                  ; shut the gate again
        call    kbcwait
        mov     al,0D0h
        out     64h,al
        mov     cx,0FFFFh
d0b_wait:
        in      al,64h
        test    al,01h
        jnz     d0b_ready
        loop    d0b_wait
        mov     dx,OFFSET msg_d0hang
        call    say_fail
        jmp     short did_d0b
d0b_ready:
        in      al,60h
        test    al,02h                  ; the gate is shut now, so clear
        jnz     d0b_bad
        mov     dx,OFFSET msg_d0again
        call    say_ok
        jmp     short did_d0b
d0b_bad:
        mov     dx,OFFSET msg_d0stale
        call    say_fail
did_d0b:

;--- DDh and DFh, the shortcut some chipsets answer directly ---------
        mov     al,0
        call    setkbcs
        mov     dx,OFFSET msg_ddoff
        mov     ah,0
        call    check

        mov     al,1
        call    setkbcs
        mov     dx,OFFSET msg_dfon
        mov     ah,1
        call    check

;=====================================================================
; Route three: ask the BIOS.  INT 15h AH=24h is the documented way,
; and it is the third road to the same gate - if it were reflected to
; the real BIOS it would move the real line behind everything else.
;=====================================================================
        mov     dx,OFFSET msg_rbios
        mov     ah,09h
        int     21h

        mov     ax,2400h                ; disable
        int     15h
        jc      bios_nofn
        mov     dx,OFFSET msg_b0
        mov     ah,0
        call    check

        mov     ax,2401h                ; enable
        int     15h
        mov     dx,OFFSET msg_b1
        mov     ah,1
        call    check

        mov     ax,2402h                ; and what do you say it is?
        int     15h
        jc      bios_getbad
        cmp     al,1
        jne     bios_getbad
        mov     dx,OFFSET msg_b2
        call    say_ok
        jmp     short did_b2
bios_getbad:
        mov     dx,OFFSET msg_b2bad
        call    say_fail
did_b2:
        jmp     short did_bios
bios_nofn:
        mov     dx,OFFSET msg_bnofn
        call    say_fail
did_bios:

;=====================================================================
; The cross-check, and it is the one this whole exercise is for: after
; the program has moved the gate itself, with no driver involved, does
; the driver agree?  XMS function 07h is "what is A20 doing", and if
; it answers out of its own bookkeeping it will say whatever it last
; set rather than what is true.
;=====================================================================
        mov     ax,4300h
        int     2Fh
        cmp     al,80h
        jne     no_xms
        mov     ax,4310h
        int     2Fh
        mov     word ptr [xms_entry],bx
        mov     word ptr [xms_entry+2],es
        push    cs
        pop     es

        mov     dx,OFFSET msg_agree
        mov     ah,09h
        int     21h

        mov     al,0                    ; shut it behind the driver's back
        call    set92
        mov     ah,07h
        call    dword ptr [xms_entry]
        or      ax,ax
        jnz     agree_bad               ; it thinks the gate is open
        mov     al,1                    ; and open it again
        call    set92
        mov     ah,07h
        call    dword ptr [xms_entry]
        or      ax,ax
        jz      agree_bad
        mov     dx,OFFSET msg_agreeok
        call    say_ok
        jmp     short did_agree
agree_bad:
        mov     dx,OFFSET msg_agreebad
        call    say_fail
        jmp     short did_agree
no_xms:
        push    cs
        pop     es
        mov     dx,OFFSET msg_noxms
        mov     ah,09h
        int     21h
did_agree:

;--- leave it as we found it -----------------------------------------
        mov     al,byte ptr [entry_a20]
        call    set92

;--- and prove the machine survived ----------------------------------
; If the interception had let the real A20 line move, the kernel would
; have lost bit 20 of every address it uses and this line would never
; be printed - the failure mode is a dead machine, not a wrong answer.
        mov     dx,OFFSET msg_alive
        mov     ah,09h
        int     21h

        mov     al,byte ptr [failed]
        mov     ah,4Ch
        int     21h

;=====================================================================
; probe - AL = 1 if the 1MB wrap is gone, 0 if it is still there.
;         Leaves both bytes as it found them.
;=====================================================================
probe:
        push    bx
        push    cx
        push    ds
        push    es
        xor     ax,ax
        mov     ds,ax
        mov     ax,0FFFFh
        mov     es,ax
        mov     bl,byte ptr ds:[500h]           ; keep what is there -
        mov     bh,byte ptr es:[510h]           ; the same byte, maybe
        mov     byte ptr ds:[500h],55h
        mov     byte ptr es:[510h],0AAh
        mov     cl,byte ptr ds:[500h]
        mov     byte ptr es:[510h],bh           ; high first, so that the
        mov     byte ptr ds:[500h],bl           ; low write wins if they
                                                ; are the same byte
        xor     ax,ax
        cmp     cl,55h
        jne     pr_out                          ; the high write landed
        mov     al,1                            ; on it: still wrapping
pr_out:
        pop     es
        pop     ds
        pop     cx
        pop     bx
        ret

;=====================================================================
; check - AH = the state expected, DX -> the message.  Probes, reports
;         and remembers a failure.
;=====================================================================
check:
        push    dx
        push    ax
        call    probe
        mov     bl,al
        pop     ax
        pop     dx
        cmp     bl,ah
        jne     ck_bad
        call    say_ok
        ret
ck_bad:
        call    say_fail
        ret

;=====================================================================
; set92 - AL = 0 or 1, through the PS/2 system control port.
;=====================================================================
set92:
        push    bx
        mov     bl,al
        in      al,92h
        and     al,0FDh
        shl     bl,1
        or      al,bl
        and     al,0FEh                 ; bit 0 resets the machine, and
        out     92h,al                  ; asking for that is not the test
        pop     bx
        ret

;=====================================================================
; setkbc - AL = 0 or 1, through the 8042's output port.
;=====================================================================
setkbc:
        push    bx
        mov     bl,al
        call    kbcwait
        mov     al,0D1h
        out     64h,al
        call    kbcwait
        mov     al,bl
        shl     al,1
        or      al,01h                  ; bit 0 low would reset the CPU
        out     60h,al
        call    kbcwait
        pop     bx
        ret

;=====================================================================
; setkbcs - AL = 0 or 1, through the DDh/DFh shortcut.
;=====================================================================
setkbcs:
        push    ax
        call    kbcwait
        pop     ax
        or      al,al
        jz      sks_off
        mov     al,0DFh
        out     64h,al
        jmp     short sks_wait
sks_off:
        mov     al,0DDh
        out     64h,al
sks_wait:
        call    kbcwait
        ret

kbcwait:
        push    ax
        push    cx
        mov     cx,0FFFFh
kw1:
        in      al,64h
        test    al,02h                  ; input buffer still full?
        jz      kw2
        loop    kw1
kw2:
        pop     cx
        pop     ax
        ret

;=====================================================================
; Output helpers
;=====================================================================
say_state:
        call    probe
        mov     dx,OFFSET msg_off
        or      al,al
        jz      ss_say
        mov     dx,OFFSET msg_on
ss_say:
        mov     ah,09h
        int     21h
        ret

say_ok:
        push    dx
        mov     dx,OFFSET msg_ok
        mov     ah,09h
        int     21h
        pop     dx
        mov     ah,09h
        int     21h
        ret

say_fail:
        mov     byte ptr [failed],1
        push    dx
        mov     dx,OFFSET msg_fail
        mov     ah,09h
        int     21h
        pop     dx
        mov     ah,09h
        int     21h
        ret

;--- data -------------------------------------------------------------
xms_entry   dd  0
entry_a20   db  0
failed      db  0

msg_hi      db  'A20TEST.COM - the A20 gate, reached through the hardware',13,10,'$'
msg_start   db  '  on entry the gate is $'
msg_on      db  'open',13,10,'$'
msg_off     db  'shut',13,10,'$'

msg_r92     db  '  port 92h:',13,10,'$'
msg_92on    db  'set: the wrap is gone',13,10,'$'
msg_92off   db  'cleared: the wrap is back',13,10,'$'
msg_92rd    db  'reads back what it was written',13,10,'$'
msg_92rdbad db  'wrote bit 1 and read it back clear',13,10,'$'

msg_rkbc    db  '  keyboard controller:',13,10,'$'
msg_kbcoff  db  'D1h output port bit 1 clear: wrap present',13,10,'$'
msg_kbcon   db  'D1h output port bit 1 set: wrap gone',13,10,'$'
msg_d0ok    db  'D0h reads the output port back correctly',13,10,'$'
msg_d0bad   db  'D0h says A20 is off when it is on',13,10,'$'
msg_d0hang  db  'D0h never produced a byte',13,10,'$'
msg_d0again db  'D0h again, gate shut: bit 1 clear, no stale state',13,10,'$'
msg_d0stale db  'D0h answered from the previous round trip',13,10,'$'
msg_ddoff   db  'DDh shortcut: wrap present',13,10,'$'
msg_dfon    db  'DFh shortcut: wrap gone',13,10,'$'

msg_rbios   db  '  INT 15h AH=24h:',13,10,'$'
msg_b0      db  '2400h disable: wrap present',13,10,'$'
msg_b1      db  '2401h enable: wrap gone',13,10,'$'
msg_b2      db  '2402h agrees the gate is open',13,10,'$'
msg_b2bad   db  '2402h disagrees with the wrap',13,10,'$'
msg_bnofn   db  'the BIOS refused AH=24h',13,10,'$'

msg_agree   db  '  and does XMS agree?',13,10,'$'
msg_agreeok db  'function 07h tracked the hardware both ways',13,10,'$'
msg_agreebad db 'function 07h answered from its own bookkeeping',13,10,'$'
msg_noxms   db  '  no XMS driver to cross-check against',13,10,'$'

msg_alive   db  'A20TEST done - and the kernel is still here',13,10,'$'
msg_ok      db  '    OK   $'
msg_fail    db  '    FAIL $'

_A20    ENDS
        END
