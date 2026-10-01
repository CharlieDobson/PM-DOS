;=====================================================================
; SHARETST.ASM - SHARETST.COM, SHARE asked the questions a program
;                that relies on it asks.
;
; SIXTEEN-BIT, like CACHETST and for the same reason: the first thing
; a program asks SHARE is INT 2Fh AX=1000h, and that lives in the DOS
; box's chain.  Everything after it is INT 21h and would work from a
; native program too.
;
; The expected answers are SHARE.EXE's, from its CUCA table:
;
;   1  2Fh/1000h says installed
;   2  compat read, then compat read again: no conflict on one machine
;   3  deny-write read while a compat read is open: SHARING VIOLATION,
;      through INT 24h with DI = 0Dh, error 20h once the handler fails
;   4  deny-write read alone, then deny-none read beside it: allowed
;   5  ...then deny-none WRITE beside them: refused (20h)
;   6  3Ch on a file open deny-all: refused BEFORE it is truncated
;   7  two deny-none read/write handles: lock 0-9 on the first
;   8  lock 5-14 on the second: LOCK VIOLATION, 21h, no INT 24h
;   9  lock 0-9 on the first again: 21h - own locks count too
;  10  read 0-3 through the second: 21h, via INT 24h with DI = 0Eh
;  11  read 0-3 through the first: fine, it is its own lock
;  12  read 10-13 through the second: fine, past the lock
;  13  unlock 0-9 on the first, then read 0-3 through the second: fine
;  14  unlock it again: 21h - nothing to unlock
;  15  SHARE=20: twenty locks, and the twenty-first is 24h
;  16  close the handle: every lock goes, and a lock fits again
;
; With SHARE=OFF in CONFIG.SYS the first check says "not installed"
; and the rest are skipped: there is nothing left to test.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_ST     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_ST, DS:_ST, ES:_ST, SS:_ST
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

        ; our own INT 24h: remember the code, answer FAIL
        push    ds
        mov     ax,2524h
        mov     dx,OFFSET crit24
        int     21h
        pop     ds

        ; the file every check opens
        mov     dx,OFFSET fname
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      no_file
        mov     bx,ax
        mov     cx,64
        mov     dx,OFFSET filler
        mov     ah,40h
        int     21h
        mov     ah,3Eh
        int     21h

;--- 1 -----------------------------------------------------------------
        mov     dx,OFFSET msg_t1
        call    puts
        mov     ax,1000h
        int     2Fh
        cmp     al,0FFh
        je      t1_yes
        mov     dx,OFFSET msg_noshare
        call    puts
        jmp     bye
t1_yes:
        call    ok

;--- 2 -----------------------------------------------------------------
        mov     dx,OFFSET msg_t2
        call    puts
        mov     al,00h                  ; compat read
        call    openf
        jc      t2_bad
        mov     [h1],ax
        mov     al,00h
        call    openf
        jc      t2_bad
        mov     [h2],ax
        call    ok
        mov     bx,[h2]
        call    closef
        jmp     short t3
t2_bad:
        call    bad

;--- 3 -----------------------------------------------------------------
t3:
        mov     dx,OFFSET msg_t3
        call    puts
        mov     byte ptr [crit_code],0FFh
        mov     al,20h                  ; deny-write read
        call    openf
        jnc     t3_bad                  ; it opened: wrong
        cmp     ax,20h
        jne     t3_bad
        cmp     byte ptr [crit_code],0Dh
        jne     t3_bad
        call    ok
        jmp     short t3_done
t3_bad:
        call    bad
        call    say_ax
t3_done:
        mov     bx,[h1]
        call    closef

;--- 4 -----------------------------------------------------------------
        mov     dx,OFFSET msg_t4
        call    puts
        mov     al,20h                  ; deny-write read
        call    openf
        jc      t4_bad
        mov     [h1],ax
        mov     al,40h                  ; deny-none read
        call    openf
        jc      t4_bad
        mov     [h2],ax
        call    ok
        jmp     short t5
t4_bad:
        call    bad
        call    say_ax

;--- 5 -----------------------------------------------------------------
t5:
        mov     dx,OFFSET msg_t5
        call    puts
        mov     byte ptr [crit_code],0FFh
        mov     al,41h                  ; deny-none write
        call    openf
        jnc     t5_bad
        cmp     ax,20h
        jne     t5_bad
        cmp     byte ptr [crit_code],0Dh
        jne     t5_bad
        call    ok
        jmp     short t5_done
t5_bad:
        call    bad
        call    say_ax
t5_done:
        mov     bx,[h1]
        call    closef
        mov     bx,[h2]
        call    closef

;--- 6 -----------------------------------------------------------------
        mov     dx,OFFSET msg_t6
        call    puts
        mov     al,10h                  ; deny-all read
        call    openf
        jc      t6_bad
        mov     [h1],ax
        mov     byte ptr [crit_code],0FFh
        mov     dx,OFFSET fname
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jnc     t6_bad
        cmp     ax,20h
        jne     t6_bad
        ; and the file is still 64 bytes long
        mov     bx,[h1]
        mov     ax,4202h
        xor     cx,cx
        xor     dx,dx
        int     21h
        cmp     ax,64
        jne     t6_bad
        call    ok
        jmp     short t6_done
t6_bad:
        call    bad
        call    say_ax
t6_done:
        mov     bx,[h1]
        call    closef

;--- 7 -----------------------------------------------------------------
        mov     dx,OFFSET msg_t7
        call    puts
        mov     al,42h                  ; deny-none read/write
        call    openf
        jc      t7_bad
        mov     [h1],ax
        mov     al,42h
        call    openf
        jc      t7_bad
        mov     [h2],ax
        mov     bx,[h1]
        mov     dx,0
        mov     di,10
        call    lockr
        jc      t7_bad
        call    ok
        jmp     short t8
t7_bad:
        call    bad
        call    say_ax

;--- 8 -----------------------------------------------------------------
t8:
        mov     dx,OFFSET msg_t8
        call    puts
        mov     byte ptr [crit_code],0FFh
        mov     bx,[h2]
        mov     dx,5
        mov     di,10
        call    lockr
        jnc     t8_bad
        cmp     ax,21h
        jne     t8_bad
        cmp     byte ptr [crit_code],0FFh       ; no INT 24h for 5Ch
        jne     t8_bad
        call    ok
        jmp     short t9
t8_bad:
        call    bad
        call    say_ax

;--- 9 -----------------------------------------------------------------
t9:
        mov     dx,OFFSET msg_t9
        call    puts
        mov     bx,[h1]
        mov     dx,0
        mov     di,10
        call    lockr
        jnc     t9_bad
        cmp     ax,21h
        jne     t9_bad
        call    ok
        jmp     short t10
t9_bad:
        call    bad
        call    say_ax

;--- 10 ----------------------------------------------------------------
t10:
        mov     dx,OFFSET msg_t10
        call    puts
        mov     byte ptr [crit_code],0FFh
        mov     bx,[h2]
        xor     dx,dx
        call    readat
        jnc     t10_bad
        cmp     ax,21h
        jne     t10_bad
        cmp     byte ptr [crit_code],0Eh
        jne     t10_bad
        call    ok
        jmp     short t11
t10_bad:
        call    bad
        call    say_ax

;--- 11 ----------------------------------------------------------------
t11:
        mov     dx,OFFSET msg_t11
        call    puts
        mov     bx,[h1]
        xor     dx,dx
        call    readat
        jc      t11_bad
        call    ok
        jmp     short t12
t11_bad:
        call    bad
        call    say_ax

;--- 12 ----------------------------------------------------------------
t12:
        mov     dx,OFFSET msg_t12
        call    puts
        mov     bx,[h2]
        mov     dx,10
        call    readat
        jc      t12_bad
        call    ok
        jmp     short t13
t12_bad:
        call    bad
        call    say_ax

;--- 13 ----------------------------------------------------------------
t13:
        mov     dx,OFFSET msg_t13
        call    puts
        mov     bx,[h1]
        mov     dx,0
        mov     di,10
        call    unlockr
        jc      t13_bad
        mov     bx,[h2]
        xor     dx,dx
        call    readat
        jc      t13_bad
        call    ok
        jmp     short t14
t13_bad:
        call    bad
        call    say_ax

;--- 14 ----------------------------------------------------------------
t14:
        mov     dx,OFFSET msg_t14
        call    puts
        mov     bx,[h1]
        mov     dx,0
        mov     di,10
        call    unlockr
        jnc     t14_bad
        cmp     ax,21h
        jne     t14_bad
        call    ok
        jmp     short t15
t14_bad:
        call    bad
        call    say_ax

;--- 15 ----------------------------------------------------------------
t15:
        mov     dx,OFFSET msg_t15
        call    puts
        mov     bx,[h1]
        mov     cx,20
        xor     dx,dx
t15_lp:
        push    cx
        mov     di,1
        call    lockr                    ; one byte each, at 100, 101, ...
        pop     cx
        jc      t15_bad
        inc     dx
        loop    t15_lp
        mov     di,1
        call    lockr                    ; the twenty-first
        jnc     t15_bad
        cmp     ax,24h
        jne     t15_bad
        call    ok
        jmp     short t16
t15_bad:
        call    bad
        call    say_ax

;--- 16 ----------------------------------------------------------------
t16:
        mov     dx,OFFSET msg_t16
        call    puts
        mov     bx,[h1]
        call    closef
        mov     al,42h
        call    openf
        jc      t16_bad
        mov     [h1],ax
        mov     bx,ax
        mov     dx,0
        mov     di,10
        call    lockr
        jc      t16_bad
        call    ok
        jmp     short t16_done
t16_bad:
        call    bad
        call    say_ax
t16_done:
        mov     bx,[h1]
        call    closef
        mov     bx,[h2]
        call    closef

bye:
        mov     dx,OFFSET fname
        mov     ah,41h
        int     21h
        mov     dx,OFFSET msg_done
        call    puts
        mov     al,[failures]
        call    put2d
        mov     dx,OFFSET msg_failed
        call    puts
        mov     ax,4C00h
        int     21h

no_file:
        mov     dx,OFFSET msg_nofile
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; the pieces
;=====================================================================

; openf - AL = the mode byte -> AX = handle, CF and AX = error
openf:
        mov     dx,OFFSET fname
        mov     ah,3Dh
        int     21h
        ret

closef:
        mov     ah,3Eh
        int     21h
        ret

; lockr/unlockr - BX = handle, DX = the first byte (below 64K), DI = how
;               many.  CF and AX on a refusal.
lockr:
        mov     ax,5C00h
        jmp     short lk_go
unlockr:
        mov     ax,5C01h
lk_go:
        xor     cx,cx
        xor     si,si
        int     21h
        ret

; readat - BX = handle, DX = position: four bytes from there.  CF and
;          AX on a refusal.
readat:
        push    dx
        mov     ax,4200h
        xor     cx,cx
        int     21h
        pop     dx
        mov     dx,OFFSET scratch
        mov     cx,4
        mov     ah,3Fh
        int     21h
        ret

; crit24 - the handler: the code in DI is kept, the answer is FAIL
crit24:
        mov     ax,di                  ; DI carries the code
        mov     cs:[crit_code],al
        mov     al,3                   ; fail
        iret

say_ax:
        push    dx
        mov     dx,OFFSET msg_ax
        call    puts
        call    put4
        mov     dx,OFFSET msg_crit
        call    puts
        mov     al,[crit_code]
        call    put2
        call    crlf
        pop     dx
        ret

ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    ax
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        pop     dx
        pop     ax
        inc     byte ptr [failures]
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
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret

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

put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
        call    put2
        ret

put2d:
        push    ax
        push    bx
        push    cx
        push    dx
        movzx   ax,al
        mov     bx,10
        xor     cx,cx
d_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     d_div
d_put:
        pop     ax
        add     al,'0'
        mov     dl,al
        call    putc
        loop    d_put
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

failures    db  0
crit_code   db  0FFh
h1          dw  0
h2          dw  0
fname       db  'C:\SHARETMP.$$$',0
filler      db  64 dup('x')
scratch     db  16 dup(0)

msg_hi      db  13,10,'SHARETST - file sharing and record locking',13,10
            db  '-------------------------------------------',13,10,'$'
msg_t1      db  '2Fh/1000h installed?          $'
msg_t2      db  'compat read, twice            $'
msg_t3      db  'deny-write beside compat: 20h $'
msg_t4      db  'deny-write + deny-none read   $'
msg_t5      db  '...+ deny-none write: 20h     $'
msg_t6      db  '3Ch on a deny-all file: 20h   $'
msg_t7      db  'two r/w handles, lock 0-9     $'
msg_t8      db  'lock 5-14 on the other: 21h   $'
msg_t9      db  'lock 0-9 again, same one: 21h $'
msg_t10     db  'read 0-3 on the other: 21h    $'
msg_t11     db  'read 0-3 on the locker        $'
msg_t12     db  'read 10-13 on the other       $'
msg_t13     db  'unlock, then read on the other$'
msg_t14     db  'unlock again: 21h             $'
msg_t15     db  'twenty locks, then 24h        $'
msg_t16     db  'close frees them all          $'
msg_ok      db  'OK',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_ax      db  '    AX=$'
msg_crit    db  ' INT 24h code=$'
msg_noshare db  'not installed (SHARE=OFF?) - nothing to test',13,10,'$'
msg_nofile  db  'cannot create C:\SHARETMP.$$$',13,10,'$'
msg_done    db  13,10,'SHARETST done, $'
msg_failed  db  ' failed',13,10,'$'
msg_crlf    db  13,10,'$'

_ST     ENDS
        END     start
