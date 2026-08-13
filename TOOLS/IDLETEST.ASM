;=====================================================================
; IDLETEST.ASM - IDLETEST.COM, the INT 28h idle hook
;
; A 16-bit DOS program that hooks INT 28h, waits for a key, and counts
; how many times its handler ran while it waited.  That count IS the
; test: before v0.31 the kernel blocked inside the keyboard driver on
; a HLT and INT 28h was never issued at all, so a resident program had
; no moment at which it was allowed to call DOS back - which is the
; whole mechanism a pop-up TSR and a background print spooler are
; built on.
;
; Two things are being proved at once, and the second is the one that
; could have gone wrong:
;
;   that INT 28h is issued, and reaches a REAL-MODE handler.  The
;   kernel is 32-bit protected-mode code and the handler is 16-bit
;   real-mode code in a DOS box, so getting there means an excursion
;   down into V86 and back - PM-DOS borrows the box's own monitor loop
;   to do it, the same trick DPMI's 0300h uses
;
;   that doing it repeatedly does not corrupt the waiting program.
;   The excursion runs on the guest's own stack with the guest's own
;   registers, and comes back to a program that is still sitting
;   inside its INT 21h.  Eighteen of those a second for as long as
;   somebody takes to press a key is a great deal of opportunity to
;   get it wrong, so the key that finally arrives is checked, and so
;   is a signature left in the registers across the wait.
;
;   IDLETEST        wait for a key (press one)
;   IDLETEST /T     do not wait: report and exit, for a batch file
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_IT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_IT, DS:_IT, ES:_IT, SS:_IT
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

;--- hook INT 28h ----------------------------------------------------
; 25h from a 16-bit program writes the REAL-MODE vector table, which
; is where the monitor reflects untrapped interrupts through - so this
; is a genuine real-mode hook and not a protected-mode one wearing its
; name.
        mov     ax,3528h
        int     21h
        mov     word ptr [old28],bx
        mov     word ptr [old28+2],es

        mov     dx,OFFSET i28
        mov     ax,2528h
        int     21h

;--- was it /T? ------------------------------------------------------
        mov     si,81h
        movzx   cx,byte ptr ds:[80h]
        xor     bl,bl
it_tail:
        or      cx,cx
        jz      it_go
        mov     al,[si]
        cmp     al,'/'
        jne     it_next
        mov     bl,[si+1]
        or      bl,20h
it_next:
        inc     si
        dec     cx
        jmp     short it_tail

it_go:
        cmp     bl,'t'
        je      it_nowait

;--- wait for a key --------------------------------------------------
; A signature goes into the registers that are NOT the ones 08h uses,
; so that an excursion which came back having trodden on the guest's
; context is caught rather than assumed against.
        mov     dx,OFFSET msg_press
        call    puts
        mov     bx,05A5Ah
        mov     si,0C3C3h
        mov     di,03C3Ch
        mov     bp,0F00Dh
        mov     ah,08h
        int     21h
        mov     [gotkey],al
        mov     [gotbx],bx
        mov     [gotsi],si
        mov     [gotdi],di
        mov     [gotbp],bp
        jmp     short it_report

it_nowait:
        ; /T never waits, so the hook never fires, so there is nothing
        ; to conclude from a count of zero.  It reports and says so
        ; rather than checking - a test that cannot pass must not be
        ; allowed to fail either, or the batch file that runs it stops
        ; meaning anything.
        mov     byte ptr [skipchk],1
        mov     dx,OFFSET msg_notwait
        call    puts
        mov     [gotbx],05A5Ah
        mov     [gotsi],0C3C3h
        mov     [gotdi],03C3Ch
        mov     [gotbp],0F00Dh

it_report:
;--- put the vector back BEFORE printing anything --------------------
; The handler lives in this program's memory and this program is about
; to exit.  A vector left pointing here is a vector pointing at
; whatever is loaded next.
        push    ds
        lds     dx,dword ptr [old28]
        mov     ax,2528h
        int     21h
        pop     ds

        mov     dx,OFFSET msg_count
        call    puts
        mov     ax,[count]
        call    put4
        call    crlf

        cmp     byte ptr [skipchk],0
        jne     it_done

        ; The count is the test.  Zero means the hook was never
        ; called, which is exactly the state v0.31 set out to change.
        mov     dx,OFFSET t_fired
        call    puts
        cmp     word ptr [count],0
        je      it_nofire
        call    good
        jmp     short it_regs
it_nofire:
        call    bad

it_regs:
        mov     dx,OFFSET t_regs
        call    puts
        cmp     word ptr [gotbx],05A5Ah
        jne     it_regbad
        cmp     word ptr [gotsi],0C3C3h
        jne     it_regbad
        cmp     word ptr [gotdi],03C3Ch
        jne     it_regbad
        cmp     word ptr [gotbp],0F00Dh
        jne     it_regbad
        call    good
        jmp     short it_done
it_regbad:
        call    bad

it_done:
        mov     dx,OFFSET msg_bye
        call    puts
        movzx   ax,byte ptr [fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; The handler.  CS: on everything, because an interrupt handler is
; entered with whatever DS the interrupted code had - and PM-DOS
; deliberately hands INT 28h the guest's own live registers, so DS
; here is the DS of whatever this program last set, which is not a
; thing to bet a counter on.
;=====================================================================
i28:
        inc     word ptr cs:[count]
        iret

;=====================================================================
; helpers
;=====================================================================
good:
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
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret
puts:
        push    ax
        mov     ah,09h
        int     21h
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
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
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
msg_hi      db  13,10,'IDLETEST - INT 28h, the hook a resident program waits on',13,10,'$'
msg_press   db  '  hooked INT 28h.  Press a key...',13,10,'$'
msg_notwait db  '  /T: not waiting, so the count below will be small',13,10,'$'
msg_count   db  '  INT 28h handler ran $'
msg_crlf    db  ' times',13,10,'$'
msg_bye     db  'IDLETEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
t_fired     db  '  the idle hook was called at all$'
t_regs      db  '  the wait gave the registers back unchanged$'

old28       dd  0
count       dw  0
gotkey      db  0
fails       db  0
skipchk     db  0
            db  0
gotbx       dw  0
gotsi       dw  0
gotdi       dw  0
gotbp       dw  0

_IT     ENDS
        END
