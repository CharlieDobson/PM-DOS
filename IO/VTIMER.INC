;=====================================================================
; VTIMER.INC - the timer a DOS box believes in
;
; THE TIMER'S INTERRUPT WAS THE KERNEL'S AND NOBODY ELSE'S.  irq_timer
; kept 40:6C, ran the floppy motor down and went back, and a DOS box
; never saw INT 08h - so INT 1Ch never happened either.  A resident
; clock, a screen blanker, a music player and every game that paces
; itself off the timer hooked one of the two and was never called.
;
; So every tick is now OWED to the box as well (vpc_tick), and goes in
; through the box's interrupt controller like a card's interrupt does
; (VPIC.INC): at the vector the box's first controller is based at,
; when the box's interrupt flag is set, in service until the EOI.
;
; THE BIOS'S OWN HANDLER IS NOT WHAT IT REACHES.  INT 08h in the box's
; vector table points at a stub of the kernel's (vt_code, planted once
; at boot), and a program that hooks INT 08h and passes it on reaches
; the stub where on DOS it would have reached the BIOS.  The stub
; calls INT 1Ch and sends the EOI - and that is all, because the rest
; of the BIOS's handler is the kernel's: 40:6C is counted in protected
; mode, where the kernel's own timeouts can see it, and the floppy
; motor belongs to the kernel's floppy calls.  Letting the ROM's
; handler run in the box as well would count every tick twice and
; switch the motor off under a transfer.
;
; IN REAL MODE THE STUB IS THE ROM'S AGAIN.  The vector table is the
; same table when the machine has been handed to Windows (a park), and
; a BIOS call the kernel makes reaches the stub as well, through
; rq_timer (IORM.ASM) and the copy of this table it was given; there
; the ROM's handler is the only thing counting.  So the stub asks
; first: SMSW says whether it is in V86 at all, and a byte of its own
; says whether the V86 monitor around it is this kernel's rather than
; Windows', and in either other case it jumps on to the ROM.
;
; NOBODY LISTENING, NOTHING DELIVERED.  With INT 08h still at the stub
; and INT 1Ch still the ROM's do-nothing IRET, a tick in the box would
; be five traps that end where they started, eighteen times a second,
; so the line is dropped instead (vt_hooked).
;
; A FASTER TIMER.  A game that wants its INT 08h seventy times a second
; programs channel 0 itself, and the box's ports to the timer go to the
; timer: what the game asked for is what it gets.  But 40:6C is the
; kernel's clock and it is counted per interrupt, so 43h and 40h are
; WATCHED (vt_out) for the divisor, and irq_timer counts a tick of the
; clock only every 65536 counts of the timer's input (vt_whole).  The
; clock keeps 18.2 Hz and the game keeps its seventy.  The ports are
; not stood in for: every byte reaches the 8254 as it was written.
; During a BIOS call the count goes on in rq_timer, which hands the
; ROM's handler whole ticks only: vt_div and vt_acc go down to it and
; come back (rq_down and rq_up, RMCALL.INC).
;
; Included by IOPM.ASM, after VPIC.INC.
;=====================================================================

;---------------------------------------------------------------------
; The stub.  Sixteen-bit code, planted at VT_STUB in the paragraphs
; IOS_BUSINIT is given; the two fields are at fixed offsets in it.
;
;   00  50                push    ax
;   01  0F 01 E0          smsw    ax
;   04  A8 01             test    al,1            ; real mode:
;   06  74 12             jz      rom             ; the ROM's
;   08  2E 80 3E ll hh 00 cmp     byte ptr cs:[ours],0
;   0E  74 0A             je      rom             ; Windows': the ROM's
;   10  58                pop     ax
;   11  CD 1C             int     1Ch
;   13  50                push    ax
;   14  B0 20             mov     al,20h
;   16  E6 20             out     20h,al          ; to the box's controller
;   18  58                pop     ax
;   19  CF                iret
;   1A  58          rom:  pop     ax
;   1B  2E FF 2E ll hh    jmp     dword ptr cs:[romvec]
;   20  ours    db 1
;   21          db 0
;   22  romvec  dd ?
;---------------------------------------------------------------------
VTS_OURS        EQU     20h
VTS_ROM         EQU     22h
VTS_LEN         EQU     26h
.ERRE   (VT_STUB + VTS_LEN) LE (BUS_PARAS * 16), <the INT 08h stub does not fit in BUS_PARAS>

vt_code         db      50h, 0Fh, 01h, 0E0h, 0A8h, 01h, 74h, 12h
                db      2Eh, 80h, 3Eh
                dw      VT_STUB + VTS_OURS
                db      00h, 74h, 0Ah
                db      58h, 0CDh, 1Ch, 50h, 0B0h, 20h, 0E6h, 20h, 58h, 0CFh
                db      58h, 2Eh, 0FFh, 2Eh
                dw      VT_STUB + VTS_ROM
                db      1, 0
                dd      0
.ERRNZ  ($ - vt_code) - VTS_LEN, <vt_code is not VTS_LEN bytes>

;=====================================================================
; vt_setup - AX = the bus paragraphs' segment, from IOS_BUSINIT, or 0.
;            Plant the stub and point INT 08h at it - once, like every
;            vector the kernel plants, so that a resident program that
;            hooks it stays hooked.  The ROM's handler is what the stub
;            hands real mode to.
;=====================================================================
vt_setup:
        pushad
        movzx   eax,ax
        or      eax,eax
        jz      vtu_none
        mov     edi,eax
        shl     edi,4
        add     edi,VT_STUB
        mov     dword ptr [vt_flat],edi
        mov     esi,OFFSET vt_code
        mov     ecx,VTS_LEN
        cld
        rep     movsb
        mov     edi,dword ptr [vt_flat]
        mov     ebx,dword ptr ds:[08h * 4]
        mov     dword ptr [edi+VTS_ROM],ebx
        shl     eax,16
        mov     ax,VT_STUB
        mov     dword ptr [vt_entry],eax
        mov     dword ptr ds:[08h * 4],eax
        mov     dword ptr [vpc_boot],eax        ; "not hooked" is now the stub
vtu_none:
        popad
        ret

;=====================================================================
; vt_park - AL = 0: the machine is about to be handed to real mode, and
;           whatever V86 monitor runs a DOS program next is not this
;           one; AL = 1: it is back.
;
; Back means the timer is as somebody else left it.  Windows puts it
; back to 18.2 Hz on its way out and nothing here watched it in the
; meantime, so the count is believed to be the BIOS's again.
;=====================================================================
vt_park:
        push    edi
        mov     edi,dword ptr [vt_flat]
        or      edi,edi
        jz      vtp_none
        mov     byte ptr [edi+VTS_OURS],al
vtp_none:
        or      al,al
        jz      vtp_out
        mov     dword ptr [vt_div],PIT_FULL
        mov     dword ptr [vt_acc],0
        mov     byte ptr [vt_flip],0
vtp_out:
        pop     edi
        ret

;=====================================================================
; vt_tick - irq_timer, once an interrupt.  CF clear if the clock at
;           40:6C has a whole tick to count, CF set if not yet.
;=====================================================================
vt_tick:
        push    eax
        mov     eax,dword ptr [vt_div]
        add     eax,dword ptr [vt_acc]
        sub     eax,PIT_FULL
        jb      vtt_part
        mov     dword ptr [vt_acc],eax
        pop     eax
        clc
        ret
vtt_part:
        add     eax,PIT_FULL
        mov     dword ptr [vt_acc],eax
        pop     eax
        stc
        ret

;=====================================================================
; vt_hooked - CF clear if anybody in the box is listening to the
;             timer: INT 08h is not the stub, or INT 1Ch is not what
;             the BIOS left there.  EDX = the vector line 0 is at in
;             the box.  Everything preserved.
;
; With no stub there is nothing to listen with - a tick delivered to
; the ROM's handler in the box would count 40:6C twice - so the answer
; is no.
;=====================================================================
vt_hooked:
        push    eax
        cmp     dword ptr [vt_entry],0
        je      vth_no
        mov     eax,dword ptr ds:[edx*4]
        cmp     eax,dword ptr [vt_entry]
        jne     vth_yes
        mov     eax,dword ptr ds:[1Ch * 4]
        cmp     eax,dword ptr [vt_boot1c]
        je      vth_no
vth_yes:
        pop     eax
        clc
        ret
vth_no:
        pop     eax
        stc
        ret

;=====================================================================
; vt_out - DX = 40h or 43h, AL = a byte for it, from vio_out.  It goes
;          to the timer as written; what it says about channel 0's
;          divisor is kept.
;
; A control word for channel 0 that is not a latch command says how
; the count will be written - its low byte, its high byte, or both in
; that order - and starts that order again.  The divisor is changed
; when the last byte of the count is in, which is when the timer
; itself changes it.  Every other control word is for another channel
; or reads something back, and changes nothing here.
;=====================================================================
vt_out:
        out     dx,al
        push    eax
        cmp     dx,PIT_CTRL
        je      vto_ctrl
        mov     ah,byte ptr [vt_access]
        cmp     ah,1
        je      vto_low
        cmp     ah,2
        je      vto_high
        cmp     ah,3
        jne     vto_out
        xor     byte ptr [vt_flip],1
        jz      vto_both                ; the second of the two
        mov     byte ptr [vt_lsb],al
        jmp     short vto_out
vto_both:
        mov     ah,al
        mov     al,byte ptr [vt_lsb]
        jmp     short vto_set
vto_low:
        xor     ah,ah
        jmp     short vto_set
vto_high:
        mov     ah,al
        xor     al,al
vto_set:
        movzx   eax,ax
        or      eax,eax
        jnz     vto_div
        mov     eax,PIT_FULL            ; a count of 0 is 65536
vto_div:
        mov     dword ptr [vt_div],eax
        jmp     short vto_out
vto_ctrl:
        test    al,0C0h                 ; channel 0, and not the read-back
        jnz     vto_out
        test    al,30h                  ; not a latch command
        jz      vto_out
        shr     al,4
        and     al,3
        mov     byte ptr [vt_access],al
        mov     byte ptr [vt_flip],0
vto_out:
        pop     eax
        ret

;=====================================================================
; State
;=====================================================================
vt_div          dd      PIT_FULL                ; channel 0's divisor
vt_acc          dd      0                       ; counts of it towards a tick
vt_entry        dd      0                       ; seg:off of the stub, 0 = none
vt_flat         dd      0                       ; ...and where it is
vt_boot1c       dd      0                       ; INT 1Ch as the BIOS left it
vt_access       db      3                       ; how the count is written:
                                                ; 1 low, 2 high, 3 low then high
vt_flip         db      0                       ; 1 = the low byte is in
vt_lsb          db      0
vt_whole        db      0                       ; this interrupt was a tick
