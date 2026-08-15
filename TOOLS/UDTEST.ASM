;=====================================================================
; UDTEST.ASM - UDTEST.COM, does a 16-bit program get its OWN exception?
;
; Vectors 0 to 7 are not crashes on an 8086.  They are how the CPU
; talks to the program: divide by zero goes to INT 00h, BOUND to
; INT 05h, an instruction the chip has never heard of to INT 06h, and
; a DOS program that hooks one of those expects to be told.
;
; Until v0.35 the V86 monitor emulated #GP and killed the DOS box for
; anything else, so a program taking any of these was reported as
; "the guest faulted, vector 06" and ended - with its own handler
; installed and waiting, one instruction away.
;
; That is not an obscure case.  It is how EVERY CPU detector written
; before CPUID was universal asks its question: hook INT 06h, run the
; instruction, and see whether it comes back.  QCONFIG on a 486 does
; exactly this, which is why QCONFIG "crashed" - it did not crash, it
; asked, and the kernel answered by killing it.
;
;   1  INT 06h  an undefined opcode reaches the program's handler
;   2  and the handler is told WHERE, so it can step over it
;   3  INT 00h  divide by zero, the oldest one of all
;   4  INT 04h  INTO with the overflow flag set - one byte the monitor
;               did not know, which used to be "faulted" as well
;   5  the runaway guard: a handler that never gets past the faulting
;      instruction must end the BOX, not the machine.  Separate,
;      because it cannot be survived - run UDTEST /HANG for it.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_UD     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_UD, DS:_UD, ES:_UD, SS:_UD
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

        cmp     byte ptr ds:[80h],0     ; a command tail at all?
        je      ud_go
        mov     si,81h
sc_lp:
        lodsb
        cmp     al,0Dh
        je      ud_go
        cmp     al,'/'
        jne     sc_lp
        lodsb
        or      al,20h
        cmp     al,'h'
        je      ud_hang
        jmp     short sc_lp

;=====================================================================
; 1 and 2 - the undefined opcode.  0F 0B is not an instruction on a
;     386, a 486 or anything else that runs DOS, so this raises #UD
;     wherever it is run - no CPU-model guessing, and no risk that the
;     test passes by accident on a chip that happens to have CPUID.
;
;     The handler does what a real one does: notes that it happened,
;     notes WHERE, and steps the return address over the two bytes so
;     that the program carries on.  Check 2 is that "where", because a
;     handler given the wrong address cannot step over anything and
;     the whole technique rests on it.
;=====================================================================
ud_go:
        mov     ax,3506h                ; get, so we can put it back
        int     21h
        mov     [old6_off],bx
        mov     [old6_seg],es
        push    ds
        mov     dx,OFFSET int6_hook
        mov     ax,2506h
        int     21h
        pop     ds

        mov     dx,OFFSET t_ud
        call    puts
        mov     bx,1234h                ; something to prove survived
ud_op:
        db      0Fh,0Bh                 ; the opcode nobody implements
        cmp     byte ptr [ud_hit],0
        je      ud_no
        cmp     bx,1234h                ; ...and the registers came back
        jne     ud_no
        call    ok
        jmp     short ud_where
ud_no:
        call    bad

ud_where:
        mov     dx,OFFSET t_udip
        call    puts
        mov     ax,[ud_ip]
        call    put4
        mov     dl,'/'
        call    putc
        mov     ax,OFFSET ud_op
        call    put4
        mov     ax,[ud_ip]
        cmp     ax,OFFSET ud_op
        jne     ud_w_no
        call    ok
        jmp     short ud_div
ud_w_no:
        call    bad

;=====================================================================
; 3 - divide by zero.  DIV BL is two bytes, and the address pushed is
;     the DIV itself, so the handler steps over it the same way.
;=====================================================================
ud_div:
        mov     ax,3500h
        int     21h
        mov     [old0_off],bx
        mov     [old0_seg],es
        push    ds
        mov     dx,OFFSET int0_hook
        mov     ax,2500h
        int     21h
        pop     ds

        mov     dx,OFFSET t_div
        call    puts
        mov     ax,0FFFFh
        mov     bl,0
dv_op:
        div     bl                      ; F6 F3 - two bytes
        cmp     byte ptr [dv_hit],0
        je      dv_no
        call    ok
        jmp     short ud_into
dv_no:
        call    bad

;=====================================================================
; 4 - INTO.  One byte, CEh, and it only does anything when the
;     overflow flag is set - so the flag is set on purpose first, with
;     an addition that genuinely overflows rather than by poking
;     FLAGS.  The address INTO pushes is the one AFTER it, unlike the
;     two above: it is a trap, not a fault, and the handler here has
;     nothing to step over.
;=====================================================================
ud_into:
        mov     ax,3504h
        int     21h
        mov     [old4_off],bx
        mov     [old4_seg],es
        push    ds
        mov     dx,OFFSET int4_hook
        mov     ax,2504h
        int     21h
        pop     ds

        mov     dx,OFFSET t_into
        call    puts
        mov     al,127
        add     al,1                    ; signed overflow: OF is now set
        into
        cmp     byte ptr [in_hit],0
        je      in_no
        call    ok
        jmp     short ud_done
in_no:
        call    bad

ud_done:
        call    unhook
        mov     dx,OFFSET msg_bye
        call    puts
        mov     al,[fails]
        or      al,al
        jz      ud_exit
        push    ax
        mov     dx,OFFSET msg_fails
        call    puts
        pop     ax
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts
ud_exit:
        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; 5 - /HANG.  INT 06h is pointed at a bare IRET and then an undefined
;     opcode is executed, so the handler returns to the instruction
;     that faulted and takes the same fault again, for ever.
;
;     THIS IS WHAT THE ROM'S OWN DUMMY HANDLER DOES, so it is the
;     state any program reaches that faults without hooking anything.
;     On real hardware it is a locked machine and the power switch.
;     Here the monitor counts the identical faults, gives up, and ends
;     the box: expect "the guest is stuck on its own handler for
;     vector 06" and a prompt.
;
;     A PASS IS THE MACHINE STILL BEING ALIVE.  Nothing below prints.
;=====================================================================
ud_hang:
        mov     dx,OFFSET msg_hang
        call    puts
        push    ds
        mov     dx,OFFSET bare_iret
        mov     ax,2506h
        int     21h
        pop     ds
        db      0Fh,0Bh                 ; ...and round, and round
        mov     dx,OFFSET msg_nohang    ; only reached if the monitor let
        call    puts                    ; it through, which is a failure
        mov     ax,4C01h
        int     21h

bare_iret:
        iret

;=====================================================================
; the handlers
;=====================================================================
; INT 06h - a fault: the stacked IP is the instruction itself, and 0F 0B
; is two bytes wide.  BP is the only register touched, and it is saved,
; because a handler that returns different registers from the ones it
; was entered with is a bug in the program, not in the kernel.
int6_hook:
        push    bp
        mov     bp,sp
        push    ax
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     byte ptr [ud_hit],1
        mov     ax,[bp+2]               ; bp, then IP, CS, FLAGS
        mov     [ud_ip],ax
        add     word ptr [bp+2],2
        pop     ds
        pop     ax
        pop     bp
        iret

; INT 00h - the same shape.  DIV BL is two bytes as well.
int0_hook:
        push    bp
        mov     bp,sp
        push    ax
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     byte ptr [dv_hit],1
        add     word ptr [bp+2],2
        pop     ds
        pop     ax
        pop     bp
        iret

; INT 04h - a trap: nothing to step over
int4_hook:
        push    ax
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     byte ptr [in_hit],1
        pop     ds
        pop     ax
        iret

;=====================================================================
; helpers
;=====================================================================
; unhook - put back everything that was taken, whether or not it was
; used.  A vector left pointing into a program that has exited is the
; classic way to make the NEXT program's failure look like this one's.
unhook:
        push    ds
        mov     dx,[old6_off]
        mov     ds,[old6_seg]
        mov     ax,2506h
        int     21h
        pop     ds
        push    ds
        mov     dx,[old0_off]
        mov     ds,[old0_seg]
        mov     ax,2500h
        int     21h
        pop     ds
        push    ds
        mov     dx,[old4_off]
        mov     ds,[old4_seg]
        mov     ax,2504h
        int     21h
        pop     ds
        ret

ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
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
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:                                   ; AL in hex
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
; data
;=====================================================================
old6_off    dw  0
old6_seg    dw  0
old0_off    dw  0
old0_seg    dw  0
old4_off    dw  0
old4_seg    dw  0
ud_hit      db  0
dv_hit      db  0
in_hit      db  0
ud_ip       dw  0
fails       db  0

msg_hi      db  13,10,'UDTEST - does a program get its own exceptions?',13,10,13,10,'$'
msg_bye     db  13,10,'UDTEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_fails   db  'checks failed: $'
msg_hang    db  'INT 06h now points at a bare IRET, and here comes an',13,10
            db  'undefined opcode.  The DOS box should END, with a reason;',13,10
            db  'the MACHINE should not.',13,10,13,10,'$'
msg_nohang  db  'FAILED - the undefined opcode did not fault at all',13,10,'$'

t_ud        db  '  1 INT 06h reached my handler        $'
t_udip      db  '  2 ...and was told where: got/want   $'
t_div       db  '  3 INT 00h divide by zero            $'
t_into      db  '  4 INT 04h from INTO                 $'

_UD     ENDS
        END     start
