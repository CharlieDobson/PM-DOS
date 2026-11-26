;=====================================================================
; VIFTEST.ASM - VIFTEST.COM: a DOS box's interrupt flag, as the box
;               sees it.
;
; A box runs at IOPL 0 and never really turns interrupts off: its CLI,
; STI, PUSHF, POPF, IRET and INT are answered by the monitor - or, on a
; processor with VME, by the processor itself - and the flag they work
; on is a virtual one.  This checks that the box cannot tell:
;
;   1  CLI and STI, read back with PUSHF
;   2  POPF puts the flag back, on and off
;   3  IRET puts it back
;   4  INT to a handler of this program's own: the handler starts with
;      the flag off, and the IRET gives the caller its own back
;   5  the same with the frame across a page boundary
;   6  PUSHF and POPF with SP at 0, where the stack wraps
;   7  a timer tick that arrives while the flag is off waits for it
;   8  ...and is handed over AT the STI that turns it on, not a tick later
;   9  ...and at a POPF that turns it on
;  10  ...and at an IRET that turns it on
;  11  INT 1Ah AH=00h, a BIOS call reflected through the vector table,
;      gives the tick count at 40:6Ch
;  12  the flags test every CPU detector runs says 386 or later
;
; The last line says whether 16-bit PUSHF shows IOPL 3 - which is what a
; processor answering these instructions itself (VME) does - so the
; output is the same on every run of one machine.  The exit code is the
; number of checks that failed.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_VT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_VT, DS:_VT, ES:_VT, SS:_VT
        ORG     0

TEST_VEC        EQU     0F1h            ; a vector nobody uses
EFL_IF          EQU     0200h

start:
        cld
        mov     ax,3500h + TEST_VEC
        int     21h
        mov     word ptr [oldvec],bx
        mov     word ptr [oldvec+2],es
        mov     ax,2500h + TEST_VEC
        mov     dx,OFFSET in_handler
        int     21h
        mov     ax,3508h
        int     21h
        mov     word ptr [old08],bx
        mov     word ptr [old08+2],es
        push    cs
        pop     es

;--- 1: CLI and STI --------------------------------------------------
        mov     byte ptr [num],1
        cli
        pushf
        pop     ax
        sti
        pushf
        pop     bx
        test    ax,EFL_IF
        jnz     c1_bad
        test    bx,EFL_IF
        jz      c1_bad
        call    ok
        jmp     short c2
c1_bad:
        call    bad

;--- 2: POPF, on and off ---------------------------------------------
c2:
        mov     byte ptr [num],2
        cli
        pushf
        pop     ax
        or      ax,EFL_IF
        push    ax
        popf                            ; on
        pushf
        pop     bx
        and     ax,NOT EFL_IF
        push    ax
        popf                            ; off
        pushf
        pop     cx
        sti
        test    bx,EFL_IF
        jz      c2_bad
        test    cx,EFL_IF
        jnz     c2_bad
        call    ok
        jmp     short c3
c2_bad:
        call    bad

;--- 3: IRET puts the flag back --------------------------------------
c3:
        mov     byte ptr [num],3
        cli
        pushf
        pop     ax
        or      ax,EFL_IF
        push    ax                      ; FLAGS, with the flag on
        push    cs
        push    OFFSET c3_back
        iret
c3_back:
        pushf
        pop     bx
        test    bx,EFL_IF
        jz      c3_bad
        call    ok
        jmp     short c4
c3_bad:
        sti
        call    bad

;--- 4: INT to a handler of our own ----------------------------------
c4:
        mov     byte ptr [num],4
        sti
        mov     word ptr [inflags],0FFFFh
        int     TEST_VEC
        pushf
        pop     bx
        cli
        int     TEST_VEC
        pushf
        pop     cx
        mov     dx,word ptr [inflags]   ; what the second call saw
        sti
        test    dx,EFL_IF
        jnz     c4_bad                  ; the handler starts with it off
        test    bx,EFL_IF
        jz      c4_bad                  ; the first caller had it on
        test    cx,EFL_IF
        jnz     c4_bad                  ; the second had it off
        call    ok
        jmp     short c5
c4_bad:
        call    bad

;--- 5: the same frame across a page boundary ------------------------
; SP is put where the six bytes the INT pushes straddle a 4K page of
; linear memory, inside this program's own 64K.
c5:
        mov     byte ptr [num],5
        mov     ax,cs
        movzx   eax,ax
        shl     eax,4                   ; the segment's linear base
        mov     edx,eax
        add     eax,0F000h              ; a page boundary in the segment's
        and     eax,NOT 0FFFh           ; top 64K - 4K, comfortably above
        sub     eax,edx                 ; this program
        add     eax,3                   ; three bytes above it: FLAGS and CS
        mov     word ptr [c5sp],ax      ; above, IP below
        mov     word ptr [savesp],sp
        cli
        mov     sp,ax
        sti
        int     TEST_VEC
        sub     sp,2                    ; and a PUSHF and POPF whose word
        pushf                           ; straddles it too
        popf
        add     sp,2
        cli
        mov     ax,sp
        mov     sp,word ptr [savesp]
        sti
        cmp     ax,word ptr [c5sp]
        jne     c5_bad
        test    word ptr [inflags],EFL_IF
        jnz     c5_bad
        call    ok
        jmp     short c6
c5_bad:
        call    bad

;--- 6: SP at 0 ------------------------------------------------------
; The PUSHF writes the top word of the segment, FFFEh, which in a .COM
; is this program's own stack: SP starts there.
c6:
        mov     byte ptr [num],6
        mov     word ptr [savesp],sp
        cli
        xor     sp,sp
        pushf                           ; IF off, at FFFEh
        mov     ax,sp
        popf
        mov     bx,sp
        mov     sp,word ptr [savesp]
        sti
        cmp     ax,0FFFEh
        jne     c6_bad
        or      bx,bx
        jnz     c6_bad
        call    ok
        jmp     short c7
c6_bad:
        call    bad

;--- 7-10: the timer -------------------------------------------------
c7:
        mov     ax,2508h
        mov     dx,OFFSET tick_handler
        int     21h

        mov     byte ptr [num],7
        cli
        call    wait_tick               ; a tick is owed now
        mov     ax,word ptr [ticks]
        call    wait_tick               ; and another, still off
        mov     bx,word ptr [ticks]
        sti
        call    wait_tick               ; on, and a tick or two is seen
        call    wait_tick
        mov     cx,word ptr [ticks]
        cmp     ax,bx
        jne     c7_bad                  ; one got in with the flag off
        cmp     cx,bx
        je      c7_bad                  ; none got in with it on
        call    ok
        jmp     short c8
c7_bad:
        call    bad

c8:
        mov     byte ptr [num],8
        cli
        call    wait_tick               ; owed
        mov     ax,word ptr [ticks]
        sti                             ; ...given here
        mov     bx,word ptr [ticks]     ; so this sees it
        cmp     ax,bx
        je      c8_bad
        call    ok
        jmp     short c9
c8_bad:
        call    bad

c9:
        mov     byte ptr [num],9
        pushf                           ; on
        cli
        call    wait_tick
        mov     ax,word ptr [ticks]
        popf                            ; ...given here
        mov     bx,word ptr [ticks]
        cmp     ax,bx
        je      c9_bad
        call    ok
        jmp     short c10
c9_bad:
        call    bad

c10:
        mov     byte ptr [num],10
        sti
        pushf
        pop     dx                      ; FLAGS with the flag on
        cli
        call    wait_tick
        mov     ax,word ptr [ticks]
        push    dx
        push    cs
        push    OFFSET c10_back
        iret                            ; ...given here
c10_back:
        mov     bx,word ptr [ticks]
        sti
        cmp     ax,bx
        je      c10_bad
        call    ok
        jmp     short c11
c10_bad:
        call    bad

c11:
        push    ds
        lds     dx,dword ptr [old08]
        mov     ax,2508h
        int     21h
        pop     ds

;--- 11: INT 1Ah through the vector table ----------------------------
        mov     byte ptr [num],11
        push    fs
        xor     ax,ax
        mov     fs,ax
        mov     ebx,dword ptr fs:[46Ch]
        xor     ah,ah
        int     1Ah                     ; CX:DX = the ticks
        mov     esi,dword ptr fs:[46Ch]
        pop     fs
        shl     ecx,16
        mov     cx,dx
        cmp     ecx,ebx
        jb      c11_bad
        cmp     ecx,esi
        ja      c11_bad
        call    ok
        jmp     short c12
c11_bad:
        call    bad

;--- 12: the CPU detectors' flags test -------------------------------
; Bits 12-15 always set: an 8086.  Never settable: a 286.  Else 386+.
c12:
        mov     byte ptr [num],12
        pushf
        pop     ax
        mov     cx,ax
        and     ax,0FFFh
        push    ax
        popf
        pushf
        pop     ax
        and     ax,0F000h
        cmp     ax,0F000h
        je      c12_bad                 ; "an 8086"
        mov     ax,cx
        or      ax,7000h
        push    ax
        popf
        pushf
        pop     ax
        push    cx
        popf
        test    ax,7000h
        jz      c12_bad                 ; "a 286"
        call    ok
        jmp     short c_end
c12_bad:
        call    bad

c_end:
        mov     dx,OFFSET msgIopl
        mov     ah,09h
        int     21h
        pushf
        pop     ax
        mov     dx,OFFSET msgNo
        test    ah,30h
        jz      c_say
        mov     dx,OFFSET msgYes
c_say:
        mov     ah,09h
        int     21h

        push    ds
        lds     dx,dword ptr [oldvec]
        mov     ax,2500h + TEST_VEC
        int     21h
        pop     ds
        mov     al,byte ptr [fails]
        mov     ah,4Ch
        int     21h

;---------------------------------------------------------------------
; wait_tick - until 40:6Ch changes, whatever the interrupt flag says:
;             the kernel keeps the count, not this box.
;---------------------------------------------------------------------
wait_tick:
        push    fs
        push    eax
        xor     ax,ax
        mov     fs,ax
        mov     eax,dword ptr fs:[46Ch]
wt_lp:
        cmp     eax,dword ptr fs:[46Ch]
        je      wt_lp
        pop     eax
        pop     fs
        ret

;--- the handlers ----------------------------------------------------
in_handler:
        push    ax
        pushf
        pop     ax
        mov     word ptr cs:[inflags],ax
        pop     ax
        iret

tick_handler:
        push    ax
        inc     word ptr cs:[ticks]
        mov     al,20h
        out     20h,al
        pop     ax
        iret

;--- ok / bad - say how check [num] went -----------------------------
ok:
        mov     dx,OFFSET msgOk
        jmp     short say
bad:
        inc     byte ptr [fails]
        mov     dx,OFFSET msgBad
say:
        push    dx
        mov     dx,OFFSET msgCheck
        mov     ah,09h
        int     21h
        mov     al,byte ptr [num]
        aam
        add     ax,3030h
        cmp     ah,30h
        jne     say2
        mov     ah,' '
say2:
        xchg    al,ah                   ; tens first
        mov     word ptr [numtxt],ax
        mov     dx,OFFSET numtxt
        mov     ah,09h
        int     21h
        pop     dx
        mov     ah,09h
        int     21h
        ret

msgCheck        db      'check $'
numtxt          db      '  $'
msgOk           db      ' ok',13,10,'$'
msgBad          db      ' FAILED',13,10,'$'
msgIopl         db      'PUSHF shows IOPL 3: $'
msgYes          db      'yes',13,10,'$'
msgNo           db      'no',13,10,'$'

oldvec          dd      0
old08           dd      0
inflags         dw      0
ticks           dw      0
savesp          dw      0
c5sp            dw      0
num             db      0
fails           db      0

_VT     ENDS
        END     start
