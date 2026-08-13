;=====================================================================
; XMSTEST.ASM - XMSTEST.COM, a 16-bit DOS program that exercises the
;               extended memory manager and the A20 gate.
;
; It knows nothing about how any of it is provided.  It finds XMS the
; way every DOS program finds XMS - INT 2Fh AX=4300h, then AX=4310h
; for a far pointer to call - and from there it is an ordinary client
; of an ordinary driver.  That is the whole point of the exercise.
;
; The A20 checks are the interesting ones, because they are the ones
; that cannot be answered by returning a number.  A 16-bit far pointer
; reaches 10FFEFh at most, and on an 8086 it did not reach that far at
; all: FFFF:0510 came out at 00500h.  So the test writes at 0000:0500
; and reads at FFFF:0510 and asks whether they are the same byte.  With
; the gate shut they must be; with it open they must not.
;
; 0000:0500 is used rather than 0000:0000 for the obvious reason - the
; bottom of memory is the interrupt vector table, and a test that
; scribbles on INT 0 to prove a point is a test that takes the machine
; with it.  0500h is the scratch byte DOS itself never minded losing,
; and PM-DOS's own boot-time A20 check uses the same pair.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_XMS    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_XMS, DS:_XMS, ES:_XMS, SS:_XMS
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- is there a driver at all? ---------------------------------------
        mov     ax,4300h
        int     2Fh
        cmp     al,80h
        je      have_xms
        mov     dx,OFFSET msg_noxms
        mov     ah,09h
        int     21h
        jmp     bail

have_xms:
        mov     ax,4310h
        int     2Fh                     ; ES:BX = the entry point
        mov     word ptr [xms_entry],bx
        mov     word ptr [xms_entry+2],es
        mov     dx,OFFSET msg_at
        mov     ah,09h
        int     21h
        mov     ax,es
        call    put4
        mov     dl,':'
        mov     ah,02h
        int     21h
        mov     ax,word ptr [xms_entry]
        call    put4
        call    crlf

;--- 00h: version, and whether it claims an HMA ----------------------
        mov     ah,00h
        call    dword ptr [xms_entry]
        mov     word ptr [t_ax],ax
        mov     word ptr [t_dx],dx
        mov     dx,OFFSET msg_ver
        mov     ah,09h
        int     21h
        mov     ax,word ptr [t_ax]
        call    put4
        mov     dx,OFFSET msg_hma
        mov     ah,09h
        int     21h
        mov     ax,word ptr [t_dx]
        call    put4
        call    crlf

;--- 01h: ask for the HMA, and expect to be told there is none -------
        mov     dx,OFFSET msg_reqhma
        mov     ah,09h
        int     21h
        mov     ah,01h
        mov     dx,0FFFFh               ; the whole of it, as a TSR would
        call    dword ptr [xms_entry]
        or      ax,ax
        jnz     hma_given
        cmp     bl,90h                  ; "HMA does not exist"
        jne     hma_wrong
        mov     dx,OFFSET msg_nohma
        call    say_ok
        jmp     short did_hma
hma_given:
        mov     dx,OFFSET msg_gothma
        call    say_fail
        jmp     short did_hma
hma_wrong:
        mov     dx,OFFSET msg_hmaerr
        call    say_fail
did_hma:

;--- 08h: how much is there? -----------------------------------------
        mov     ah,08h
        call    dword ptr [xms_entry]
        mov     word ptr [t_ax],ax
        mov     word ptr [t_dx],dx
        mov     dx,OFFSET msg_free
        mov     ah,09h
        int     21h
        mov     ax,word ptr [t_ax]
        call    put5dec
        mov     dx,OFFSET msg_of
        mov     ah,09h
        int     21h
        mov     ax,word ptr [t_dx]
        call    put5dec
        mov     dx,OFFSET msg_kb
        mov     ah,09h
        int     21h

;--- 88h: and again in 32 bits, which is why XMS 3.0 exists ----------
; The pair above report kilobytes in sixteen-bit registers, so on any
; machine with more than 64MB they come back saturated at 65535 and
; say nothing useful.  This is the same question asked properly.
        mov     ah,88h
        call    dword ptr [xms_entry]
        mov     dword ptr [t_eax],eax
        mov     dword ptr [t_edx],edx
        mov     dx,OFFSET msg_free32
        mov     ah,09h
        int     21h
        mov     eax,dword ptr [t_eax]
        call    put10dec
        mov     dx,OFFSET msg_of
        mov     ah,09h
        int     21h
        mov     eax,dword ptr [t_edx]
        call    put10dec
        mov     dx,OFFSET msg_kb
        mov     ah,09h
        int     21h

;--- 09h: take 64K of it ---------------------------------------------
        mov     ah,09h
        mov     dx,64
        call    dword ptr [xms_entry]
        or      ax,ax
        jz      alloc_bad
        mov     word ptr [handle],dx
        mov     dx,OFFSET msg_got
        call    say_ok
        jmp     short did_alloc
alloc_bad:
        mov     dx,OFFSET msg_allocbad
        call    say_fail
        jmp     bail
did_alloc:

;--- 0Ch: lock it, and see the address -------------------------------
        mov     ah,0Ch
        mov     dx,word ptr [handle]
        call    dword ptr [xms_entry]
        or      ax,ax
        jz      lock_bad
        mov     word ptr [t_ax],bx
        mov     word ptr [t_dx],dx
        mov     dx,OFFSET msg_lock
        mov     ah,09h
        int     21h
        mov     ax,word ptr [t_dx]
        call    put4
        mov     ax,word ptr [t_ax]
        call    put4
        call    crlf
        mov     ah,0Dh                  ; and unlock it again
        mov     dx,word ptr [handle]
        call    dword ptr [xms_entry]
        jmp     short did_lock
lock_bad:
        mov     dx,OFFSET msg_lockbad
        call    say_fail
did_lock:

;--- 0Bh: move a string out to extended memory and back --------------
        mov     dx,OFFSET msg_move
        mov     ah,09h
        int     21h

        mov     word ptr [mv_len],32    ; the request: conventional -> EMB
        mov     word ptr [mv_len+2],0
        mov     word ptr [mv_srch],0    ; handle 0 = a far pointer
        mov     word ptr [mv_srco],OFFSET pattern
        mov     ax,cs
        mov     word ptr [mv_srco+2],ax
        mov     ax,word ptr [handle]
        mov     word ptr [mv_dsth],ax
        mov     word ptr [mv_dsto],0
        mov     word ptr [mv_dsto+2],0
        mov     si,OFFSET mv_len
        mov     ah,0Bh
        call    dword ptr [xms_entry]
        or      ax,ax
        jz      move_bad

        mov     di,OFFSET scratch       ; wipe the landing ground, so a
        mov     cx,32                   ; move that does nothing cannot
        mov     al,'.'                  ; pass by leaving the old bytes
        push    es
        push    ds
        pop     es
        rep     stosb
        pop     es

        mov     word ptr [mv_len],32    ; and back the other way
        mov     ax,word ptr [handle]
        mov     word ptr [mv_srch],ax
        mov     word ptr [mv_srco],0
        mov     word ptr [mv_srco+2],0
        mov     word ptr [mv_dsth],0
        mov     word ptr [mv_dsto],OFFSET scratch
        mov     ax,cs
        mov     word ptr [mv_dsto+2],ax
        mov     si,OFFSET mv_len
        mov     ah,0Bh
        call    dword ptr [xms_entry]
        or      ax,ax
        jz      move_bad

        mov     si,OFFSET pattern       ; did it survive the round trip?
        mov     di,OFFSET scratch
        mov     cx,32
cmp_one:
        mov     al,[si]
        cmp     al,[di]
        jne     move_wrong
        inc     si
        inc     di
        loop    cmp_one
        mov     dx,OFFSET msg_moveok
        call    say_ok
        jmp     short did_move
move_bad:
        mov     dx,OFFSET msg_movebad
        call    say_fail
        jmp     short did_move
move_wrong:
        mov     dx,OFFSET msg_movewrong
        call    say_fail
did_move:

;--- 0Ah: give it back -----------------------------------------------
        mov     ah,0Ah
        mov     dx,word ptr [handle]
        call    dword ptr [xms_entry]
        or      ax,ax
        jz      free_bad
        mov     dx,OFFSET msg_freed
        call    say_ok
        jmp     short did_free
free_bad:
        mov     dx,OFFSET msg_freebad
        call    say_fail
did_free:

;--- INT 15h AH=88h: the BIOS must say there is nothing --------------
        mov     dx,OFFSET msg_i15
        mov     ah,09h
        int     21h
        mov     ah,88h
        int     15h
        or      ax,ax
        jz      i15_ok
        mov     dx,OFFSET msg_i15bad
        call    say_fail
        jmp     short did_i15
i15_ok:
        mov     dx,OFFSET msg_i15ok
        call    say_ok
did_i15:

;=====================================================================
; A20, and the 1MB wrap it exists to switch off.
;=====================================================================
        mov     dx,OFFSET msg_a20
        mov     ah,09h
        int     21h

        mov     ax,0FFFFh               ; ES:0510 is 10FFEF+ territory
        mov     es,ax
        xor     ax,ax
        mov     ds,ax                   ; DS:0500 is the low byte
        mov     al,byte ptr ds:[500h]   ; keep what was there
        mov     byte ptr cs:[saved],al

;--- with the gate open the two must be different memory -------------
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     ah,03h                  ; global enable A20
        call    dword ptr [xms_entry]
        mov     byte ptr cs:[t_ax],al
        pop     ds

        mov     byte ptr ds:[500h],11h
        mov     byte ptr es:[510h],22h
        mov     al,byte ptr ds:[500h]
        cmp     al,11h
        je      a20_on_ok
        mov     byte ptr cs:[failed],1
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     dx,OFFSET msg_a20onbad
        call    say_fail
        pop     ds
        jmp     short a20_off
a20_on_ok:
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     dx,OFFSET msg_a20on
        call    say_ok
        pop     ds

;--- and with it shut they must be the same byte ---------------------
a20_off:
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     ah,04h                  ; global disable A20
        call    dword ptr [xms_entry]
        pop     ds

        mov     byte ptr ds:[500h],33h
        mov     al,byte ptr es:[510h]
        cmp     al,33h
        je      a20_off_ok
        mov     byte ptr cs:[failed],1
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     dx,OFFSET msg_a20offbad
        call    say_fail
        pop     ds
        jmp     short a20_done
a20_off_ok:
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     dx,OFFSET msg_a20off
        call    say_ok
        pop     ds
a20_done:
        mov     al,byte ptr cs:[saved]  ; put 0000:0500 back
        mov     byte ptr ds:[500h],al
        mov     ax,cs
        mov     ds,ax
        mov     es,ax

bail:
        mov     dx,OFFSET msg_bye
        mov     ah,09h
        int     21h
        mov     al,byte ptr [failed]
        mov     ah,4Ch
        int     21h

;--- helpers ----------------------------------------------------------
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

put10dec:                               ; EAX in decimal, no padding
        push    eax
        push    ebx
        push    ecx
        push    edx
        xor     ecx,ecx
        mov     ebx,10
p10_div:
        xor     edx,edx
        div     ebx
        push    edx
        inc     ecx
        or      eax,eax
        jnz     p10_div
p10_out:
        pop     edx
        add     dl,'0'
        push    ecx
        mov     ah,02h
        int     21h
        pop     ecx
        loop    p10_out
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

put5dec:                                ; AX in decimal, no padding
        push    bx
        push    cx
        push    dx
        xor     cx,cx
        mov     bx,10
pd_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     pd_div
pd_out:
        pop     dx
        add     dl,'0'
        push    cx
        mov     ah,02h
        int     21h
        pop     cx
        loop    pd_out
        pop     dx
        pop     cx
        pop     bx
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
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        push    ax
        mov     ah,02h
        int     21h
        pop     ax
        ret
crlf:
        mov     dl,13
        mov     ah,02h
        int     21h
        mov     dl,10
        mov     ah,02h
        int     21h
        ret

;--- data -------------------------------------------------------------
xms_entry   dd  0
handle      dw  0
t_ax        dw  0
t_dx        dw  0
t_eax       dd  0
t_edx       dd  0
saved       db  0
failed      db  0

; The move request, in the order the specification lays it out.
mv_len      dd  0
mv_srch     dw  0
mv_srco     dd  0
mv_dsth     dw  0
mv_dsto     dd  0

pattern     db  'XMS round trip, 32 bytes long!!!'
scratch     db  32 dup(0)

msg_hi      db  'XMSTEST.COM - extended memory and the A20 gate',13,10,'$'
msg_noxms   db  '  no XMS driver is installed',13,10,'$'
msg_at      db  '  entry point at $'
msg_ver     db  '  version $'
msg_hma     db  ', HMA flag $'
msg_reqhma  db  '  request the HMA: $'
msg_nohma   db  'refused, no HMA here',13,10,'$'
msg_gothma  db  'it gave us the HMA',13,10,'$'
msg_hmaerr  db  'refused with the wrong reason',13,10,'$'
msg_free    db  '  free (16-bit): largest $'
msg_free32  db  '  free (32-bit): largest $'
msg_of      db  ' KB of $'
msg_kb      db  ' KB total',13,10,'$'
msg_got     db  'allocated 64 KB',13,10,'$'
msg_allocbad db 'could not allocate 64 KB',13,10,'$'
msg_lock    db  '  locked at $'
msg_lockbad db  'could not lock the block',13,10,'$'
msg_move    db  '  moving 32 bytes out and back',13,10,'$'
msg_moveok  db  'the bytes came back intact',13,10,'$'
msg_movebad db  'the move was refused',13,10,'$'
msg_movewrong db 'the bytes came back changed',13,10,'$'
msg_freed   db  'handle freed',13,10,'$'
msg_freebad db  'could not free the handle',13,10,'$'
msg_i15     db  '  INT 15h/88h: $'
msg_i15ok   db  'no extended memory, as it should say',13,10,'$'
msg_i15bad  db  'the BIOS offered extended memory',13,10,'$'
msg_a20     db  '  the 1MB wrap:',13,10,'$'
msg_a20on   db  'A20 on: 0000:0500 and FFFF:0510 differ',13,10,'$'
msg_a20onbad db 'A20 on: they still alias',13,10,'$'
msg_a20off  db  'A20 off: 0000:0500 and FFFF:0510 alias',13,10,'$'
msg_a20offbad db 'A20 off: they did not wrap',13,10,'$'
msg_ok      db  '    OK   $'
msg_fail    db  '    FAIL $'
msg_bye     db  'XMSTEST done',13,10,'$'

_XMS    ENDS
        END
