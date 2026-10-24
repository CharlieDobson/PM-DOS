;=====================================================================
; LOCKTEST.ASM - LOCKTEST.COM, the three lock keys with nobody typing.
;
; Presses Caps Lock, Num Lock and Scroll Lock - scan codes, through
; 21h/F1h AL=23h, as KBLTEST presses its keys - and after each row
; prints the three things a lock is:
;
;   the bits at 40:17, where a program looks for it;
;
;   the lights, as the byte at 40:97 has them.  The kernel writes that
;   byte when the keyboard has acknowledged the command that lit them
;   and not before, so a row is given half a second for the two to
;   agree, and the lights are printed as they then stand;
;
;   and the keys the lock changes, read back through INT 16h.
;
; It judges nothing, and the answers are kept as a baseline.
;
; A PROGRAM MAY WRITE THE BITS ITSELF - it is how Num Lock was turned
; off before DOS 6 had NUMLOCK= - and the last two rows do, with no key
; pressed at all.  The lights have to follow those too.
;
; TWICE, as KBLTEST: with INT 09h as the kernel left it, and with a
; handler of this program's own passing each key on.  In the second
; pass it is the ROM's handler, or KEYB's stub, that turns a lock over,
; and the kernel's part is to light what it then finds at 40:17.  The
; rows that are Pause and Break are left out of that pass: a ROM stops
; at Pause until the next key, and Break is INT 1Bh.
;
; The locks are all turned off before each pass and put back as they
; were found at the end.  LOCKTEST /S says first how they were found,
; which is the BIOS's doing and CONFIG.SYS's (NUMLOCK=) and is not in
; the baseline for that reason.
;
; INT 1Bh IS THIS PROGRAM'S FOR THE RUN, and is an IRET.  DOS's own
; marks the Break key, and DOS reads the mark as a ^C at its next look
; at the keyboard - which here is the row's own output, and the default
; INT 23h ended the run there.  MS-DOS does the same; a program that
; wants Ctrl-Break as a key keeps INT 1Bh, as this one does, and reads
; the 0000h the BIOS leaves.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_LT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_LT, DS:_LT, ES:_LT, SS:_LT
        ORG     0

P_PLAIN EQU     1                       ; a row's passes: the first...
P_BOTH  EQU     3                       ; ...or the hooked one as well

start:
        call    get17
        mov     [was17],al

        mov     si,81h                  ; /S: the locks as they were found
ar_next:
        lodsb
        cmp     al,0Dh
        je      ar_done
        cmp     al,'/'
        jne     ar_next
        lodsb
        and     al,0DFh
        cmp     al,'S'
        jne     ar_done
        mov     dx,OFFSET msg_found
        call    puts
        call    put_locks
        call    crlf
ar_done:
        mov     ax,351Bh                ; the Break key, as a key
        int     21h
        mov     word ptr [old1b],bx
        mov     word ptr [old1b+2],es
        push    ds
        pop     es
        mov     dx,OFFSET int1b
        mov     ax,251Bh
        int     21h

        mov     dx,OFFSET msg_plain
        call    puts
        mov     byte ptr [pass],1
        call    run_rows

        ; the second pass: INT 09h hooked, and passed on
        mov     ax,3509h
        int     21h
        mov     word ptr [old9],bx
        mov     word ptr [old9+2],es
        push    ds
        pop     es
        mov     dx,OFFSET int9
        mov     ax,2509h
        int     21h
        mov     dx,OFFSET msg_hooked
        call    puts
        mov     byte ptr [pass],2
        call    run_rows
        push    ds
        lds     dx,dword ptr [old9]
        mov     ax,2509h
        int     21h
        pop     ds

        mov     al,[was17]              ; as they were found
        and     al,70h
        call    set_locks
        push    ds
        lds     dx,dword ptr [old1b]
        mov     ax,251Bh
        int     21h
        pop     ds
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; int9 - what a resident program's keyboard handler does: look at the
;        port, and hand the key on to whoever had the interrupt.
;---------------------------------------------------------------------
int9:
        push    ax
        in      al,60h
        pop     ax
        jmp     dword ptr cs:[old9]

int1b:
        iret

;---------------------------------------------------------------------
; run_rows - the locks off, and then each row of the table that is in
;            this pass: its name, a change written straight to 40:17 if
;            it has one, its scan codes pressed one after another, and
;            then the locks, the lights and every key INT 16h has.
;---------------------------------------------------------------------
run_rows:
        xor     al,al
        call    set_locks
        mov     si,OFFSET rows
rr_row:
        cmp     byte ptr [si],0
        je      rr_done
        mov     dx,si                   ; the name, which ends in '$'
rr_name:
        lodsb
        cmp     al,'$'
        jne     rr_name
        lodsb                           ; the passes it is in
        test    al,[pass]
        jnz     rr_do
        inc     si                      ; not this one: past the change
rr_skip:
        lodsb                           ; ...and the codes
        or      al,al
        jnz     rr_skip
        jmp     short rr_row
rr_do:
        call    puts
        mov     dl,' '
        call    putc
        lodsb                           ; bits to turn over at 40:17
        or      al,al
        jz      rr_scan
        push    es
        push    40h
        pop     es
        xor     es:[17h],al
        pop     es
rr_scan:
        lodsb
        or      al,al
        jz      rr_said
        mov     dl,al
        mov     ax,0F123h               ; a key, as if it had been pressed
        int     21h
        jmp     short rr_scan
rr_said:
        call    put_locks
rr_keys:
        mov     ah,11h                  ; anything there?
        int     16h
        jz      rr_end
        mov     ah,10h
        int     16h
        call    put4                    ; code, then character
        mov     dl,' '
        call    putc
        jmp     short rr_keys
rr_end:
        call    crlf
        jmp     short rr_row
rr_done:
        ret

;---------------------------------------------------------------------
; set_locks - AL = the three lock bits for 40:17, and wait for the
;             lights.  put_locks - wait for the lights, and print the
;             locks and then the lights.
;---------------------------------------------------------------------
set_locks:
        push    es
        push    40h
        pop     es
        and     byte ptr es:[17h],8Fh
        or      es:[17h],al
        pop     es
        call    settle
        ret

put_locks:
        call    settle
        call    get17
        and     al,70h
        call    put2
        mov     dl,' '
        call    putc
        call    get97
        and     al,07h
        call    put1
        mov     dl,' '
        call    putc
        call    putc
        ret

;---------------------------------------------------------------------
; settle - until the lights at 40:97 are the locks at 40:17, or nine
;          ticks of the clock have gone by.
;---------------------------------------------------------------------
settle:
        push    ax
        push    bx
        push    cx
        push    es
        push    40h
        pop     es
        mov     cx,9
        mov     bx,es:[6Ch]
st_look:
        mov     al,es:[17h]
        shr     al,4
        and     al,7
        mov     ah,es:[97h]
        and     ah,7
        cmp     al,ah
        je      st_out
        cmp     bx,es:[6Ch]
        je      st_look
        mov     bx,es:[6Ch]
        dec     cx
        jnz     st_look
st_out:
        pop     es
        pop     cx
        pop     bx
        pop     ax
        ret

get17:
        push    es
        push    40h
        pop     es
        mov     al,es:[17h]
        pop     es
        ret
get97:
        push    es
        push    40h
        pop     es
        mov     al,es:[97h]
        pop     es
        ret

;---------------------------------------------------------------------
; puts - DX -> a string ending in '$'; putc - DL.  Both through 40h on
;        handle 1, and not 09h and 02h, which DOS counts: every 64th
;        character it looks at the keyboard, and that look takes the
;        Break key's 0000h out of the buffer before INT 16h can show
;        it, as MS-DOS's CON does.  Where the count fell was wherever
;        the programs before this one left it.  A write to a file is
;        not counted, and the baseline's run writes to one.
;---------------------------------------------------------------------
puts:
        push    ax
        push    bx
        push    cx
        push    di
        mov     di,dx
        xor     cx,cx
ps_len:
        cmp     byte ptr [di],'$'
        je      ps_wr
        inc     di
        inc     cx
        jmp     short ps_len
ps_wr:
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret
putc:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     [pcbuf],dl
        mov     dx,OFFSET pcbuf
        mov     cx,1
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
crlf:
        push    dx
        mov     dl,13
        call    putc
        mov     dl,10
        call    putc
        pop     dx
        ret
put4:                                   ; AX as four hex digits
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
        push    dx
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_out
        add     al,7
p1_out:
        mov     dl,al
        call    putc
        pop     dx
        pop     ax
        ret

;=====================================================================
; The rows.  A name ending in '$'; the passes the row is in; a byte
; turned over at 40:17 before anything is pressed, zero for none; and
; scan codes ending in a zero - a make code, and the same code with bit
; 7 set for the key coming up.  3Ah is Caps Lock, 45h Num Lock and 46h
; Scroll Lock; 47h to 53h are the numeric keypad, and the cursor pad
; with E0h in front; 2Ah is Shift and 1Dh Ctrl.
;
; Each row begins with the locks as the row before it left them.
;=====================================================================
rows:
        db  'nothing pressed   $', P_BOTH, 0, 0
        db  'keypad 7 5 1 .    $', P_BOTH, 0, 47h,0C7h,4Ch,0CCh,4Fh,0CFh,53h,0D3h,0
        db  'num lock          $', P_BOTH, 0, 45h,0C5h,0
        db  'keypad 7 5 1 .    $', P_BOTH, 0, 47h,0C7h,4Ch,0CCh,4Fh,0CFh,53h,0D3h,0
        db  'shift keypad 7 1  $', P_BOTH, 0, 2Ah,47h,0C7h,4Fh,0CFh,0AAh,0
        db  'ctrl keypad 4 5 6 $', P_BOTH, 0, 1Dh,4Bh,0CBh,4Ch,0CCh,4Dh,0CDh,9Dh,0
        db  'keypad - +        $', P_BOTH, 0, 4Ah,0CAh,4Eh,0CEh,0
        db  'cursor left, home $', P_BOTH, 0, 0E0h,4Bh,0E0h,0CBh,0E0h,47h,0E0h,0C7h,0
        db  'num lock, held    $', P_BOTH, 0, 45h,45h,45h,45h,0C5h,0
        db  'shift keypad 7 1  $', P_BOTH, 0, 2Ah,47h,0C7h,4Fh,0CFh,0AAh,0
        db  'caps lock         $', P_BOTH, 0, 3Ah,0BAh,0
        db  'key 1E, shift 1E  $', P_BOTH, 0, 1Eh,9Eh,2Ah,1Eh,9Eh,0AAh,0
        db  'caps lock, held   $', P_BOTH, 0, 3Ah,3Ah,3Ah,3Ah,0BAh,0
        db  'key 1E            $', P_BOTH, 0, 1Eh,9Eh,0
        db  'scroll lock       $', P_BOTH, 0, 46h,0C6h,0
        db  'pause             $', P_PLAIN, 0, 0E1h,1Dh,45h,0E1h,9Dh,0C5h,0
        db  'ctrl num lock     $', P_PLAIN, 0, 1Dh,45h,0C5h,9Dh,0
        db  'ctrl break        $', P_PLAIN, 0, 1Dh,0E0h,46h,0E0h,0C6h,9Dh,0
        db  'ctrl scroll lock  $', P_PLAIN, 0, 1Dh,46h,0C6h,9Dh,0
        db  'num, ctrl, num up $', P_PLAIN, 0, 45h,1Dh,0C5h,9Dh,45h,0C5h,0
        db  'num and caps lock $', P_BOTH, 0, 45h,0C5h,3Ah,0BAh,0
        db  'written: num off  $', P_BOTH, 20h, 0
        db  'written: all off  $', P_BOTH, 50h, 0
        db  0

msg_found   db  'as found          $'
msg_plain   db  'LOCKTEST: the locks at 40:17, the lights at 40:97, and the keys',13,10,'$'
msg_hooked  db  'with INT 09h hooked, and each key passed on:',13,10,'$'
old9        dd  0
old1b       dd  0
was17       db  0
pcbuf       db  0                       ; putc's one byte
pass        db  0

_LT     ENDS
        END     start
