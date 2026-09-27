;=====================================================================
; MS32TEST.ASM - MS32TEST.EXE, the mouse driver from a native program
;
; The same driver MSTEST.COM tests from a DOS box, reached the other
; way: INT 33h at ring 3, through the gate PMIO owns.  What is different
; here is what this checks -
;
;   the registers come back WHOLE: a position that is negative is a
;   negative 32-bit number, not a 16-bit one with junk above it
;
;   a pointer is an offset in EDX, measured against the slot, and one
;   that runs off the end is refused rather than followed
;
;   the event handler is called at ring 3, in this program, with the
;   Microsoft registers and a far return - and when it returns, the
;   program carries on from where the event caught it with every
;   register as it was.  Twice: once on the way out of an INT 21h, and
;   once from a clock tick that stops the program in a loop of its own,
;   which is the path a program that never calls DOS depends on
;
;   21h/F0h AL = 8 wakes for the mouse when it is asked to
;
;   MS32TEST H    reset, install a handler, show the cursor, and leave
;   MS32TEST C    after an H: they went with it
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_MN     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MN, DS:_MN, ES:_MN, SS:_MN
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET mn_entry>,<0>,4096

MS      MACRO   fn
        mov     eax,fn
        int     33h
        ENDM

INJ     MACRO   btn, mx, my
        mov     ebx,btn
        mov     ecx,mx
        mov     edx,my
        mov     eax,0F00Bh
        int     21h
        ENDM

EXPECT     MACRO   got, want
        mov     eax,got
        mov     ebx,want
        call    chk
        ENDM

SAY     MACRO   txt
        mov     edx,OFFSET txt
        call    puts
        ENDM

mn_entry:
        mov     esi,SLOT_PSP + 81h
tail_sp:
        lodsb
        cmp     al,' '
        je      tail_sp
        and     al,0DFh
        cmp     al,'H'
        je      run_leave
        cmp     al,'C'
        je      run_check

        SAY     msg_hi

        SAY     t_reset
        MS      0
        mov     esi,ebx
        EXPECT     eax,0FFFFh
        EXPECT     esi,2
        call    verdict

        SAY     t_ver
        MS      24h
        mov     esi,ecx
        EXPECT     ebx,0820h
        cmp     esi,0200h               ; serial: type 2, IRQ 3 or 4
        jb      ver_ps2
        cmp     esi,0204h
        jbe     ver_done
ver_ps2:
        EXPECT     esi,0400h            ; PS/2: type 4, IRQ 0
ver_done:
        call    verdict

        SAY     t_centre
        MS      3
        EXPECT     ebx,0
        EXPECT     ecx,320
        EXPECT     edx,96
        call    verdict

        ; doubling off, for arithmetic that is Microsoft's plain one
        mov     edx,7FFFh
        MS      13h

        SAY     t_move
        INJ     0, 16, 16
        MS      3
        EXPECT     ecx,336
        EXPECT     edx,104
        call    verdict

;--- negative numbers are negative numbers ---------------------------
        SAY     t_neg
        mov     ecx,-100
        mov     edx,639
        MS      7
        mov     ecx,-50
        mov     edx,10
        MS      4
        MS      3
        EXPECT     ecx,-56                 ; a whole cell, rounded down
        MS      31h
        EXPECT     eax,-100
        MS      0Bh                     ; what is counted so far, gone
        INJ     0, -8, 0
        MS      0Bh
        EXPECT     ecx,-8
        xor     ecx,ecx
        mov     edx,639
        MS      7
        call    verdict

;--- a pointer is an offset, and only this program's -----------------
        SAY     t_ptr
        MS      15h
        mov     [statelen],ebx
        mov     ecx,111
        mov     edx,57
        MS      4
        mov     edx,OFFSET statebuf
        MS      16h
        xor     ecx,ecx
        xor     edx,edx
        MS      4
        mov     edx,OFFSET statebuf
        MS      17h
        MS      3
        EXPECT     ecx,104
        EXPECT     edx,56
        ; and one that runs off the end of the slot changes nothing
        mov     edx,PROG_SIZE - 8
        MS      17h
        MS      3
        EXPECT     ecx,104
        mov     edx,7FFFFFF0h
        MS      9
        MS      3
        EXPECT     ecx,104
        call    verdict

;--- the handler, on the way out of an INT 21h ------------------------
        SAY     t_event
        MS      0
        mov     edx,7FFFh
        MS      13h
        mov     ecx,320
        mov     edx,96
        MS      4
        mov     ecx,7Fh
        mov     edx,OFFSET handler
        MS      0Ch
        mov     dword ptr [h_count],0
        mov     [save_esp],esp
        mov     ebp,5A5A5A5Ah           ; what the program is holding
        mov     esi,11111111h
        mov     edi,22222222h
        INJ     0, 16, 0                ; ...and the handler runs here
        mov     [r_ebp],ebp
        mov     [r_esi],esi
        mov     [r_edi],edi
        mov     [r_ebx],ebx
        mov     [r_ecx],ecx
        mov     [r_edx],edx
        mov     [r_esp],esp
        EXPECT     [h_count],1
        EXPECT     [h_eax],1
        EXPECT     [h_ebx],0
        EXPECT     [h_ecx],336
        EXPECT     [h_edx],96
        EXPECT     [h_esi],16
        EXPECT     [h_edi],0
        mov     eax,cs
        EXPECT     [h_cs],eax              ; called in this program, at ring 3
        call    verdict

        SAY     t_back
        EXPECT     [r_ebp],5A5A5A5Ah
        EXPECT     [r_esi],11111111h
        EXPECT     [r_edi],22222222h
        EXPECT     [r_ebx],0               ; and what the INT 21h returned
        EXPECT     [r_ecx],16
        EXPECT     [r_edx],0
        mov     eax,[save_esp]
        EXPECT     [r_esp],eax
        call    verdict

;--- ...and from a clock tick, in a loop that never calls DOS ---------
; The handler injects a second event while it is still running: that
; one has to wait, because a handler is never re-entered, and nothing
; below makes an INT 21h to deliver it - so it arrives through a timer
; interrupt catching this loop at ring 3.
        SAY     t_tick
        mov     dword ptr [h_count],0
        mov     byte ptr [h_again],1
        INJ     0, 16, 0
        mov     eax,[h_count]
        mov     [r_count1],eax          ; only the first, so far
        mov     ecx,400000000
tick_wait:
        cmp     dword ptr [h_count],2
        jae     tick_done
        dec     ecx
        jnz     tick_wait
tick_done:
        EXPECT     [r_count1],1
        EXPECT     [h_inner],1             ; not re-entered from inside
        EXPECT     [h_count],2
        EXPECT     [h_eax],1
        EXPECT     [h_ecx],360
        call    verdict

        SAY     t_mask
        mov     ecx,2
        mov     edx,OFFSET handler
        MS      0Ch
        mov     dword ptr [h_count],0
        INJ     0, 16, 0
        EXPECT     [h_count],0
        INJ     1, 0, 0
        EXPECT     [h_count],1
        EXPECT     [h_eax],2
        EXPECT     [h_ebx],1
        INJ     0, 0, 0
        call    verdict

        SAY     t_swap
        xor     ecx,ecx
        xor     edx,edx
        MS      14h
        EXPECT     ecx,2
        EXPECT     edx,<OFFSET handler>
        INJ     1, 0, 0
        INJ     0, 0, 0
        EXPECT     [h_count],1
        call    verdict

;--- 21h/F0h AL = 8 wakes for the mouse, when asked to ---------------
        SAY     t_wake
        mov     eax,0F008h
        mov     edx,0300h               ; no wait, and the mouse counts
        int     21h
        INJ     0, 8, 0
        mov     eax,0F008h
        mov     edx,0300h
        int     21h
        movzx   eax,al
        mov     [r_wake1],eax
        mov     eax,0F008h
        mov     edx,0300h
        int     21h
        movzx   eax,al
        mov     [r_wake2],eax
        INJ     0, 8, 0
        mov     eax,0F008h
        mov     edx,0100h               ; not asked: a key or a shift only
        int     21h
        movzx   eax,al
        EXPECT     [r_wake1],2
        EXPECT     [r_wake2],0
        EXPECT     eax,0
        call    verdict

        SAY     t_off
        MS      1Fh
        mov     esi,ebx
        EXPECT     eax,1Fh
        EXPECT     esi,0
        MS      3
        mov     [r_ecx],ecx
        INJ     0, 16, 0
        MS      3
        EXPECT     ecx,[r_ecx]
        MS      20h
        call    verdict

        MS      0
        movzx   eax,byte ptr [fails]
        push    eax
        SAY     msg_bye
        pop     eax
        mov     ah,4Ch
        int     21h

;=====================================================================
run_leave:
        MS      0
        mov     ecx,7Fh
        mov     edx,OFFSET handler
        MS      0Ch
        MS      1
        SAY     msg_left
        mov     eax,4C00h
        int     21h

run_check:
        SAY     t_gone
        MS      2Ah
        EXPECT     eax,-1
        xor     ecx,ecx
        xor     edx,edx
        MS      14h
        EXPECT     ecx,0
        EXPECT     edx,0
        call    verdict
        movzx   eax,byte ptr [fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; The event handler: a far call, at ring 3, in this program.  It may
; destroy every register - the driver brings the program's back.
;=====================================================================
handler:
        mov     [h_eax],eax
        mov     [h_ebx],ebx
        mov     [h_ecx],ecx
        mov     [h_edx],edx
        mov     [h_esi],esi
        mov     [h_edi],edi
        mov     eax,cs
        mov     [h_cs],eax
        inc     dword ptr [h_count]
        cmp     byte ptr [h_again],0
        je      hd_out
        mov     byte ptr [h_again],0
        INJ     0, 8, 0                 ; another event, while this one runs
        mov     eax,[h_count]
        mov     [h_inner],eax           ; still 1: it was not delivered here
hd_out:
        xor     eax,eax
        mov     ebx,eax
        mov     ecx,eax
        mov     edx,eax
        mov     esi,eax
        mov     edi,eax
        mov     ebp,eax
        retf

;=====================================================================
; helpers - chk keeps the first difference on a line, verdict reports
;=====================================================================
chk:
        cmp     eax,ebx
        je      chk_out
        cmp     byte ptr [miss],0
        jne     chk_out
        mov     byte ptr [miss],1
        mov     [miss_got],eax
        mov     [miss_want],ebx
        mov     al,[chkn]
        mov     [miss_n],al
chk_out:
        inc     byte ptr [chkn]
        ret

verdict:
        pushad
        cmp     byte ptr [miss],0
        jne     vd_bad
        SAY     msg_ok
        jmp     short vd_reset
vd_bad:
        SAY     msg_chk
        movzx   eax,byte ptr [miss_n]
        call    puthex
        SAY     msg_got
        mov     eax,[miss_got]
        call    puthex
        SAY     msg_want
        mov     eax,[miss_want]
        call    puthex
        SAY     msg_bad
        inc     byte ptr [fails]
vd_reset:
        mov     byte ptr [miss],0
        mov     byte ptr [chkn],0
        popad
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; EAX in hex, eight digits
puthex:
        pushad
        mov     edi,OFFSET numbuf
        mov     ecx,8
ph_one:
        rol     eax,4
        mov     bl,al
        and     bl,0Fh
        add     bl,'0'
        cmp     bl,'9'
        jbe     ph_dig
        add     bl,7
ph_dig:
        mov     [edi],bl
        inc     edi
        loop    ph_one
        mov     byte ptr [edi],'$'
        mov     edx,OFFSET numbuf
        call    puts
        popad
        ret

;=====================================================================
msg_hi      db  13,10,'MS32TEST - INT 33h from a native program',13,10,'$'
msg_bye     db  'MS32TEST done.',13,10,'$'
msg_left    db  'MS32TEST H: a handler and a cursor left behind',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_chk     db  ' #$'
msg_got     db  ' got $'
msg_want    db  ' want $'

t_reset     db  '  00h reset: EAX 0000FFFFh, two buttons$'
t_ver       db  '  24h version 8.20, and the type$'
t_centre    db  '  03h after a reset$'
t_move      db  '  movement$'
t_neg       db  '  negative positions and counts are 32-bit negative$'
t_ptr       db  '  pointers are offsets, and the slot ends them$'
t_event     db  '  0Ch handler at ring 3 on the way out of INT 21h$'
t_back      db  '  and the program resumes with its registers$'
t_tick      db  '  a second event waits, then comes from a clock tick$'
t_mask      db  '  0Ch mask chooses the events$'
t_swap      db  '  14h swaps the handler out$'
t_wake      db  '  21h/F0h AL=8 wakes for the mouse when asked$'
t_off       db  '  1Fh/20h disable and enable$'
t_gone      db  '  MS32TEST H left nothing behind$'

fails       db  0
miss        db  0
chkn        db  0
miss_n      db  0
h_again     db  0
            db  0,0,0
miss_got    dd  0
miss_want   dd  0
save_esp    dd  0
statelen    dd  0
h_count     dd  0
h_inner     dd  0
h_eax       dd  0
h_ebx       dd  0
h_ecx       dd  0
h_edx       dd  0
h_esi       dd  0
h_edi       dd  0
h_cs        dd  0
r_ebp       dd  0
r_esi       dd  0
r_edi       dd  0
r_ebx       dd  0
r_ecx       dd  0
r_edx       dd  0
r_esp       dd  0
r_count1    dd  0
r_wake1     dd  0
r_wake2     dd  0
numbuf      db  12 dup(0)
statebuf    db  512 dup(0)

_MN     ENDS
        END
