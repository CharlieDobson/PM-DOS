;=====================================================================
; NULLTRAP.INC - catching a write through a null pointer
;
; WHY THIS IS NOT A PAGE, which is what the roadmap said it would be.
;
; The obvious implementation is one page-table entry: mark linear
; 00000000h-00000FFFh not present and every null dereference becomes a
; page fault with CR2 = the offset, which is as good as this gets.
; That is what a real operating system does and it is what the v0.30
; notes promised.
;
; It cannot be done here, and the reason is worth writing down because
; it is not an implementation detail - it is the shape of the system.
; Page zero is not empty in PM-DOS.  It holds:
;
;   0000h-03FFh  the real-mode interrupt vector table, which the DOS
;                box's guests read and write constantly
;   0400h-04FFh  the BIOS data area, which v0.27 made the AUTHORITY for
;                console geometry, the display page and the tick count
;   04F6h        the critical-error flag
;   04F7h        InDOS - incremented on EVERY SINGLE INT 21h, by the
;                third instruction of int21_entry
;   0500h        RMFRAME, the register frame every real-mode thunk
;                marshals its arguments through
;
; So unmapping page zero faults on the next DOS call the kernel makes,
; and on the timer interrupt, and it cannot be worked around by mapping
; it back on kernel entry: that is a CR3 reload and a TLB flush on every
; INT 21h and every IRQ, to protect an address range the kernel is
; obliged to keep using anyway.  InDOS in particular cannot move - 34h
; hands out its ADDRESS and the TSR that asks holds a real-mode far
; pointer, so it has to live under 1MB.
;
; The flat identity map is what makes those five things work, and the
; whole first page is load-bearing.  A program only gets a guard page
; at zero when it stops sharing an address space with DOS's low memory
; - which is the ring-3 release, where a native program runs out of its
; own LDT with a non-zero base and gets this for free.
;
; WHAT THIS DOES INSTEAD.  Four debug registers, DR0-DR3, watching
; linear 0, 4, 8 and 12 for WRITES of four bytes each.  Sixteen bytes
; rather than four thousand, and it is worth being exact about what
; that does and does not buy:
;
;   caught:   *(int *)0 = x, and a store through a null STRUCT pointer
;             to any of its first four dwords - which between them are
;             the overwhelming majority of real null-pointer bugs,
;             because the field a program writes first is usually near
;             the front
;   missed:   a write further into a large structure, any READ through
;             a null pointer, and memset(NULL, 0, n) - which begins at
;             offset zero and so is caught after all
;
; The cost is nothing: no address-space change, no TLB flush, no
; per-call work.  The registers were otherwise unused.
;
; It is armed globally rather than locally (G0-G3 rather than L0-L3)
; because there are no tasks here to switch it between, and the two
; excursions that must NOT have it armed - a V86 guest and a real-mode
; thunk - disarm it explicitly.  A DOS program hooking INT 0 by writing
; the vector table directly is doing something entirely legitimate that
; would otherwise trip this, and BIOS code called through the thunk is
; not ours to police.
;
; Included by IOPM.ASM.
;=====================================================================

; DR7, assembled.  Per breakpoint i the four-bit field at 16+4i is
; LEN:RW; LEN=11b is four bytes and RW=01b is "data writes only", so
; every field is 1101b = 0Dh.  The enable bits are G0-G3 (1, 3, 5, 7 =
; 0AAh), GE (bit 9) asks the CPU for exact instruction reporting, and
; bit 10 is reserved and documented as "write as 1".
NT_DR7ON        EQU     0DDDD06AAh
NT_DR7OFF       EQU     00000400h       ; the reserved bit and nothing else

;=====================================================================
; nt_init - watch the first sixteen bytes of the address space.
;
; The addresses go in once, here, because they never change; arming and
; disarming is DR7 alone, which is one register write.
;=====================================================================
nt_init:
        push    eax
        xor     eax,eax
        mov     dr0,eax
        mov     dr7,eax                 ; disarmed while the rest is set up
        mov     al,4
        mov     dr1,eax
        mov     al,8
        mov     dr2,eax
        mov     al,12
        mov     dr3,eax
        xor     eax,eax
        mov     dr6,eax                 ; no stale hit from the BIOS
        mov     byte ptr ds:[nt_want],1
        mov     al,1
        call    nt_arm
        pop     eax
        ret

;=====================================================================
; nt_arm - IOS_NULLTRAP.  AL = 1 arm, 0 disarm, FFh disable for good.
;
; A DISARM ALWAYS WORKS; AN ARM ONLY WORKS IF THE GUARD IS WANTED.
; That asymmetry is what lets CONFIG.SYS turn the whole thing off with
; one statement: nt_want goes to zero and every subsequent arm - the
; ones vdm_done and the thunk do on their way out, which have no idea
; why they are arming - quietly does nothing.
;=====================================================================
nt_arm:
        push    eax
        cmp     al,0FFh
        je      nta_never
        or      al,al
        jz      nta_off
        cmp     byte ptr ds:[nt_want],0
        je      nta_out
        mov     eax,NT_DR7ON
        mov     dr7,eax
        mov     byte ptr ds:[nt_on],1
        jmp     short nta_out
nta_never:
        mov     byte ptr ds:[nt_want],0
nta_off:
        mov     eax,NT_DR7OFF
        mov     dr7,eax
        mov     byte ptr ds:[nt_on],0
nta_out:
        pop     eax
        ret

;=====================================================================
; nt_get - IOS_NULLGET.  AL = whether the guard is armed right now,
;          EBX = DR7 AS THE CPU HOLDS IT.
;
; The second half is the diagnostic that matters.  A guard that is
; armed and does not fire has two possible explanations - the debug
; registers were never really written, or the CPU underneath is not
; implementing breakpoints - and they are a bug here and a footnote
; about the emulator respectively.  Reading DR7 back out of the CPU
; tells them apart in one line of output, which is worth far more than
; the six instructions it costs.
;=====================================================================
nt_get:
        mov     ebx,dr7
        mov     al,byte ptr ds:[nt_on]
        ret

;=====================================================================
; nt_hit - was this #DB one of ours?  ZF clear if so, and DR6 is
;          cleared either way: the bits are sticky, and a stale one
;          makes the NEXT debug exception look like a null write.
;=====================================================================
nt_hit:
        push    ebx
        mov     eax,dr6
        xor     ebx,ebx
        mov     dr6,ebx
        test    al,0Fh                  ; B0-B3: our four breakpoints
        pop     ebx
        ret

nt_want     db  1                       ; is the guard wanted at all
nt_on       db  0                       ; is it armed at this moment
            db  0,0
