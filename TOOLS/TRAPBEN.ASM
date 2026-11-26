;=====================================================================
; TRAPBEN.ASM - TRAPBEN.COM: how many times a second a DOS box gets
;               through each of the instructions the monitor answers.
;
; Not a test with a right answer: the numbers are the machine's, and
; they are for putting two kernels side by side on the same machine.
; Each line is one instruction (or pair) in a loop, run for about a
; second by the BIOS tick count at 40:6Ch, which the kernel keeps
; whatever a box does with its interrupt flag:
;
;   loop        the loop alone, for scale
;   CLI+STI     the interrupt flag off and on
;   PUSHF+POPF  the flags saved and put back
;   PUSHF+CLI+POPF  a critical section, the way most code writes one
;   PUSHFD+POPFD  the 32-bit forms
;   INT 0F0h    a software interrupt to this program's own IRET
;   IN AL,21h   a port the kernel stands in for (the interrupt mask)
;   INT 21h     AH=19h, a DOS call all the way to the kernel and back
;   loop, 1000 Hz / 8000 Hz   the first line again, with this program's
;               own INT 08h at that rate: the loop iterations missing
;               are what the interrupts cost, each of them an IRQ, an
;               EOI to the interrupt controller and an IRET
;
; The last line says whether 16-bit PUSHF shows IOPL 3, which is what
; the processor does when it is answering CLI, STI, PUSHF and POPF
; itself (VME) instead of the kernel.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_TB     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TB, DS:_TB, ES:_TB, SS:_TB
        ORG     0

INNER           EQU     250             ; iterations between looks at the clock
RUN_TICKS       EQU     18              ; how long each line runs: about a second
TEST_VEC        EQU     0F0h            ; a vector nobody uses

start:
        cld
        mov     ax,3500h + TEST_VEC     ; keep the vector, and point it here
        int     21h
        mov     word ptr [oldvec],bx
        mov     word ptr [oldvec+2],es
        mov     ax,2500h + TEST_VEC
        mov     dx,OFFSET just_iret
        int     21h
        push    cs
        pop     es

        mov     si,OFFSET tests
tb_next:
        lodsw                           ; the routine
        or      ax,ax
        jz      tb_done
        mov     bx,ax
        lodsw                           ; and its name
        push    si
        mov     dx,ax
        mov     ah,09h
        int     21h
        call    measure                 ; EAX = how many in the second
        call    put_dec
        mov     dx,OFFSET msgPerSec
        mov     ah,09h
        int     21h
        pop     si
        jmp     tb_next

tb_done:
;--- the loop again with this program's own timer interrupt running fast:
;--- what each interrupt costs is the work the loop does not get done
        mov     ax,3508h
        int     21h
        mov     word ptr [old08],bx
        mov     word ptr [old08+2],es
        push    cs
        pop     es
        mov     ax,2508h
        mov     dx,OFFSET isr08
        int     21h
        mov     si,OFFSET rates
tb_rate:
        lodsw                           ; the divisor
        or      ax,ax
        jz      tb_rated
        call    set_pit
        lodsw                           ; and the line's name
        push    si
        mov     dx,ax
        mov     ah,09h
        int     21h
        mov     bx,OFFSET t_loop
        call    measure
        call    put_dec
        mov     dx,OFFSET msgSecInts
        mov     ah,09h
        int     21h
        movzx   eax,word ptr [isrn]     ; what the handler saw in that time
        call    put_dec
        mov     dx,OFFSET msgInts
        mov     ah,09h
        int     21h
        pop     si
        jmp     tb_rate
tb_rated:
        xor     ax,ax                   ; 65536: the BIOS's 18.2 a second
        call    set_pit
        push    ds
        lds     dx,dword ptr [old08]
        mov     ax,2508h
        int     21h
        pop     ds

        mov     dx,OFFSET msgIopl
        mov     ah,09h
        int     21h
        pushf
        pop     ax
        mov     dx,OFFSET msgNo
        test    ah,30h
        jz      tb_say
        mov     dx,OFFSET msgYes
tb_say:
        mov     ah,09h
        int     21h

        push    ds
        lds     dx,dword ptr [oldvec]
        mov     ax,2500h + TEST_VEC
        int     21h
        pop     ds
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; measure - BX = a routine that runs INNER iterations.  Wait for the
;           tick to change, then run it until RUN_TICKS more have gone
;           by.  -> EAX = iterations in that time.
;---------------------------------------------------------------------
measure:
        push    fs
        xor     ax,ax
        mov     fs,ax
        mov     eax,dword ptr fs:[46Ch]
ms_edge:
        cmp     eax,dword ptr fs:[46Ch]
        je      ms_edge
        mov     eax,dword ptr fs:[46Ch]
        mov     dword ptr [t0],eax
        mov     word ptr [isrn],0       ; the timer lines count from here
        xor     edi,edi                 ; rounds done
ms_round:
        push    bx
        call    bx
        pop     bx
        inc     edi
        mov     eax,dword ptr fs:[46Ch]
        sub     eax,dword ptr [t0]
        cmp     eax,RUN_TICKS
        jb      ms_round
        mov     eax,edi
        imul    eax,eax,INNER
        pop     fs
        ret

;--- the routines: INNER iterations each -----------------------------
t_loop:
        mov     cx,INNER
tl_lp:  nop
        loop    tl_lp
        ret

t_clisti:
        mov     cx,INNER
tc_lp:  cli
        sti
        loop    tc_lp
        ret

t_pushpop:
        mov     cx,INNER
tp_lp:  pushf
        popf
        loop    tp_lp
        ret

t_crit:
        mov     cx,INNER
tr_lp:  pushf
        cli
        popf
        loop    tr_lp
        ret

t_pushpopd:
        mov     cx,INNER
td_lp:  pushfd
        popfd
        loop    td_lp
        ret

t_int:
        mov     cx,INNER
ti_lp:  int     TEST_VEC
        loop    ti_lp
        ret

t_port:
        mov     cx,INNER
to_lp:  in      al,21h
        loop    to_lp
        ret

t_dos:
        mov     cx,INNER
tdo_lp: mov     ah,19h
        int     21h
        loop    tdo_lp
        ret

just_iret:
        iret

;--- the timer interrupt: counted, ended, and nothing else
isr08:
        push    ax
        inc     word ptr cs:[isrn]
        mov     al,20h
        out     20h,al
        pop     ax
        iret

;--- set_pit - AX = channel 0's divisor (0 = 65536)
set_pit:
        push    ax
        cli
        mov     al,36h                  ; channel 0, both bytes, mode 3
        out     43h,al
        pop     ax
        out     40h,al
        mov     al,ah
        out     40h,al
        sti
        ret

;---------------------------------------------------------------------
; put_dec - EAX in decimal, right-aligned in ten columns.
;---------------------------------------------------------------------
put_dec:
        mov     di,OFFSET numbuf + 10
        mov     byte ptr [di],'$'
        mov     ebx,10
        mov     cx,10
pd_div:
        dec     di
        xor     edx,edx
        div     ebx
        add     dl,'0'
        mov     byte ptr [di],dl
        dec     cx
        or      eax,eax
        jnz     pd_div
pd_pad:
        jcxz    pd_out
        dec     di
        mov     byte ptr [di],' '
        loop    pd_pad
pd_out:
        mov     dx,di
        mov     ah,09h
        int     21h
        ret

tests           dw      t_loop,     msgLoop
                dw      t_clisti,   msgCliSti
                dw      t_pushpop,  msgPushPop
                dw      t_crit,     msgCrit
                dw      t_pushpopd, msgPushPopD
                dw      t_int,      msgInt
                dw      t_port,     msgPort
                dw      t_dos,      msgDos
                dw      0

rates           dw      1193,       msgT1000    ; 1193182 / 1000
                dw      149,        msgT8000    ; 1193182 / 8000
                dw      0

msgT1000        db      'loop, 1000 Hz   $'
msgT8000        db      'loop, 8000 Hz   $'

msgLoop         db      'loop            $'
msgCliSti       db      'CLI+STI         $'
msgPushPop      db      'PUSHF+POPF      $'
msgCrit         db      'PUSHF+CLI+POPF  $'
msgPushPopD     db      'PUSHFD+POPFD    $'
msgInt          db      'INT 0F0h        $'
msgPort         db      'IN AL,21h       $'
msgDos          db      'INT 21h AH=19h  $'
msgPerSec       db      ' a second',13,10,'$'
msgSecInts      db      ' a second,$'
msgInts         db      ' interrupts',13,10,'$'
msgIopl         db      'PUSHF shows IOPL 3: $'
msgYes          db      'yes',13,10,'$'
msgNo           db      'no',13,10,'$'

oldvec          dd      0
old08           dd      0
isrn            dw      0
t0              dd      0
numbuf          db      11 dup (0)

_TB     ENDS
        END     start
