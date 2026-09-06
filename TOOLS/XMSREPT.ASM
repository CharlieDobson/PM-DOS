;=====================================================================
; XMSREPT.ASM - XMSREPT.COM, what RMDOS's XMS host actually offers
;
; Windows 3.1 standard mode gets ALL of its extended memory through the
; XMS entry point, and the INT 21h trace cannot see a single one of
; those calls - they are a far call into the driver, not an interrupt.
; That blind spot is the whole reason two experiments read wrong:
;
;   - Capping the pool at 16MB, 32MB and 64MB all failed IDENTICALLY
;     with "Insufficient extended memory", which was read as "not a
;     threshold" when it may simply mean the cap left nothing at all.
;   - Uncapped, DOSX hands KRNL386 a block at 04299030h - 66MB - which
;     a 286 extender cannot build a descriptor over.  Whether that
;     address is DOSX's choice or the only place the pool HAS was
;     never established.
;
; So ask the driver the same questions Windows asks, in the same order,
; and print the answers.  The last one is the one that matters: take
; the largest block and LOCK it, because lock is the only XMS call that
; reports a linear address, and that address is where Windows' memory
; would come from.  If it is above 16MB then standard mode cannot use
; it no matter what else is fixed, and the fix belongs in park_xmsgrab
; where the pool is chosen - not in the extender and not in the kernel.
;
; Run it on the parked DOS:   PARKTEST \XMSREPT.COM
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_SS     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SS, DS:_SS, ES:_SS, SS:_SS
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

;--- is there a host at all? -----------------------------------------
        mov     ax,4300h
        int     2Fh
        cmp     al,80h
        je      x_have
        mov     dx,OFFSET msg_nohost
        call    puts
        jmp     x_bye
x_have:
        mov     ax,4310h
        int     2Fh
        mov     word ptr [xentry],bx
        mov     word ptr [xentry+2],es
        mov     dx,OFFSET msg_entry
        call    puts
        mov     ax,es
        call    put4
        mov     dl,3Ah
        call    putc
        mov     ax,bx
        call    put4
        call    crlf

;--- 00h - version ---------------------------------------------------
        mov     ah,00h
        call    dword ptr [xentry]
        push    dx
        mov     dx,OFFSET msg_ver
        call    puts
        call    put4                    ; AX = version, BCD
        pop     dx
        mov     ax,dx
        push    ax
        mov     dx,OFFSET msg_hma
        call    puts
        pop     ax
        call    put4
        call    crlf

;--- 08h - the sixteen-bit question, in kilobytes --------------------
        mov     ah,08h
        xor     bl,bl
        call    dword ptr [xentry]
        push    ax
        push    dx
        mov     dx,OFFSET msg_08
        call    puts
        pop     ax                      ; DX was total
        push    ax
        call    put4
        mov     dx,OFFSET msg_kbtot
        call    puts
        pop     ax
        pop     ax                      ; AX was largest
        call    put4
        mov     dx,OFFSET msg_kb
        call    puts

;--- 88h - the same question in thirty-two bits ----------------------
        mov     ah,88h
        xor     bl,bl
        call    dword ptr [xentry]
        push    eax
        push    edx
        push    ecx
        mov     dx,OFFSET msg_88
        call    puts
        pop     ecx
        push    ecx
        mov     eax,ecx
        call    put8                    ; highest address the host knows
        mov     dx,OFFSET msg_88b
        call    puts
        pop     ecx
        pop     edx
        mov     eax,edx
        call    put8                    ; total, KB
        mov     dx,OFFSET msg_88c
        call    puts
        pop     eax
        call    put8                    ; largest, KB
        call    crlf

;=====================================================================
; And the measurement this program exists for.  Ask for the largest
; block, lock it, and print the linear address the host reports.  That
; is the address Windows' extender would be handed, and 286 protected
; mode can only reach the first sixteen megabytes of it.
;=====================================================================
        mov     ah,08h
        xor     bl,bl
        call    dword ptr [xentry]
        or      ax,ax
        jz      x_nomem
        mov     dx,ax                   ; DX = KB to allocate
        mov     [wanted],ax
        mov     ah,09h
        call    dword ptr [xentry]
        or      ax,ax
        jz      x_allocbad
        mov     [xhandle],dx

        mov     dx,OFFSET msg_got
        call    puts
        mov     ax,[wanted]
        call    put4
        mov     dx,OFFSET msg_kbh
        call    puts
        mov     ax,[xhandle]
        call    put4
        call    crlf

        mov     ah,0Ch                  ; lock - the only call that tells
        mov     dx,[xhandle]            ; the truth about WHERE
        call    dword ptr [xentry]
        or      ax,ax
        jz      x_lockbad
        push    dx
        push    bx
        mov     dx,OFFSET msg_lin
        call    puts
        pop     bx
        pop     dx
        push    dx
        push    bx
        mov     ax,dx
        call    put4
        pop     ax
        call    put4                    ; DX:BX = the linear address
        call    crlf
        pop     dx

;--- the verdict, which is a single comparison -----------------------
        cmp     dx,0100h                ; DX is the high half: >= 16MB?
        jb      x_below
        mov     dx,OFFSET msg_above
        call    puts
        jmp     short x_unlock
x_below:
        mov     dx,OFFSET msg_belowok
        call    puts
x_unlock:
        mov     ah,0Dh
        mov     dx,[xhandle]
        call    dword ptr [xentry]
x_free:
        mov     ah,0Ah
        mov     dx,[xhandle]
        call    dword ptr [xentry]
        jmp     short x_bye

x_lockbad:
        push    bx
        mov     dx,OFFSET msg_lockbad
        call    puts
        pop     ax
        mov     al,ah
        call    put2
        call    crlf
        jmp     short x_free
x_allocbad:
        push    bx
        mov     dx,OFFSET msg_allocbad
        call    puts
        pop     ax
        mov     al,ah
        call    put2
        call    crlf
        jmp     short x_bye
x_nomem:
        mov     dx,OFFSET msg_nomem
        call    puts

x_bye:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put8:
        push    eax
        shr     eax,16
        call    put4
        pop     eax
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
put1:
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_go
        add     al,7
p1_go:
        mov     dl,al
        call    putc
        pop     ax
        ret

;---------------------------------------------------------------------
xentry      dd  0
xhandle     dw  0
wanted      dw  0

msg_hi      db  13,10,'XMSREPT - what this XMS host offers, and WHERE',13,10,13,10,'$'
msg_nohost  db  'no XMS host: INT 2Fh AX=4300h did not answer 80h.',13,10,'$'
msg_entry   db  'entry point   $'
msg_ver     db  'version       $'
msg_hma     db  '   HMA flag $'
msg_08      db  '08h  total  $'
msg_kbtot   db  'K  largest  $'
msg_kb      db  'K',13,10,'$'
msg_88      db  '88h  highest $'
msg_88b     db  '  total $'
msg_88c     db  'K  largest $'
msg_got     db  13,10,'allocated $'
msg_kbh     db  'K, handle $'
msg_lin     db  'LOCKED AT LINEAR $'
msg_above   db  13,10,'THAT IS AT OR ABOVE 16MB.  Windows 3.1 standard mode is the',13,10
            db  '286 extender and cannot address it: the pool is in the wrong',13,10
            db  'place, and park_xmsgrab is what has to choose differently.',13,10,'$'
msg_belowok db  13,10,'Below 16MB - reachable by a 286 extender.  If standard mode',13,10
            db  'still fails, it is not because of where the memory is.',13,10,'$'
msg_lockbad db  'lock FAILED, BL=$'
msg_allocbad db 'allocate FAILED, BL=$'
msg_nomem   db  'the host reports NO free extended memory at all.',13,10
            db  'That alone is "Insufficient extended memory to run Windows".',13,10,'$'
msg_bye     db  13,10,'XMSREPT done',13,10,'$'
msg_crlf    db  13,10,'$'

_SS     ENDS
        END     start
