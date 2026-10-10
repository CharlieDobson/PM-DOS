;=====================================================================
; KBLTEST.ASM - KBLTEST.COM, the keyboard's layout with nobody typing.
;
; Presses keys - scan codes, through 21h/F1h AL=23h, which the kernel
; takes exactly as it takes one off the keyboard's port - and prints
; what INT 16h then has to say: the character and the code of each key
; that came out.  It judges nothing.  The same keys mean different
; characters under every layout, so what is right is what the layout
; says, and the answer is kept as a baseline for each: the US keyboard
; with no KEYB, and one run after KEYB for each layout tried.
;
; THE KEYS ARE NAMED BY WHERE THEY ARE, not by what is printed on them
; - "key 15" is the sixth letter key of the top row, Y to an American
; and Z to a German.
;
; TWICE, because there are two ways a key reaches INT 16h's buffer and
; KEYB has to be right in both:
;
;   with INT 09h as the kernel left it - the kernel decodes the key
;   itself;
;
;   with a handler of this program's own on INT 09h, which reads the
;   port and passes the key on, as a resident program does.  What it
;   passes it on to was the ROM until KEYB loaded a layout, and is the
;   kernel's stub after (IO\KEYBD.INC, kb_hlt).
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_KT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_KT, DS:_KT, ES:_KT, SS:_KT
        ORG     0

start:
        mov     dx,OFFSET msg_plain
        call    puts
        call    run_cases

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
        call    run_cases
        push    ds
        lds     dx,dword ptr [old9]
        mov     ax,2509h
        int     21h
        pop     ds

        mov     dx,OFFSET msg_count
        call    puts
        mov     ax,[n_int9]
        call    put4
        call    crlf
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; int9 - what a resident program's keyboard handler does: look at the
;        port, and hand the key on to whoever had the interrupt.
;---------------------------------------------------------------------
int9:
        push    ax
        in      al,60h
        inc     word ptr cs:[n_int9]
        pop     ax
        jmp     dword ptr cs:[old9]

;---------------------------------------------------------------------
; run_cases - each row of the table: its name, its scan codes pressed
;             one after another, and then every key INT 16h has.
;---------------------------------------------------------------------
run_cases:
        mov     si,OFFSET cases
rc_case:
        cmp     byte ptr [si],0
        je      rc_done
        mov     dx,si                   ; the name, which ends in '$'
        call    puts
        mov     dl,' '
        call    putc
rc_name:
        lodsb
        cmp     al,'$'
        jne     rc_name
rc_scan:
        lodsb
        or      al,al
        jz      rc_keys
        mov     dl,al
        mov     ax,0F123h               ; a key, as if it had been pressed
        int     21h
        jmp     short rc_scan
rc_keys:
        mov     ah,11h                  ; anything there?
        int     16h
        jz      rc_end
        mov     ah,10h
        int     16h
        call    put4                    ; code, then character
        mov     dl,' '
        call    putc
        jmp     short rc_keys
rc_end:
        call    crlf
        jmp     short rc_case
rc_done:
        ret

puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret
putc:
        push    ax
        mov     ah,02h
        int     21h
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
; The keys.  A name ending in '$', then scan codes ending in a zero:
; a make code, and the same code with bit 7 set for the key coming up.
; E0h goes in front of a key of the cursor pad, and of the right-hand
; Alt (38h).  2Ah is Shift, 1Dh Ctrl, 38h Alt, 3Ah Caps Lock.
;=====================================================================
cases:
        db  'key 15            $', 15h,95h,0
        db  'key 2C            $', 2Ch,0ACh,0
        db  'shift 15          $', 2Ah,15h,95h,0AAh,0
        db  'key 03, shifted   $', 03h,83h,2Ah,03h,83h,0AAh,0
        db  'key 04, shifted   $', 04h,84h,2Ah,04h,84h,0AAh,0
        db  'altgr 10          $', 0E0h,38h,10h,90h,0E0h,0B8h,0
        db  'ctrl alt 10       $', 1Dh,38h,10h,90h,0B8h,9Dh,0
        db  'altgr 08 09 0A 0B $', 0E0h,38h,08h,88h,09h,89h,0Ah,8Ah,0Bh,8Bh,0E0h,0B8h,0
        db  'altgr 0C 1B       $', 0E0h,38h,0Ch,8Ch,1Bh,9Bh,0E0h,0B8h,0
        db  'altgr 1E          $', 0E0h,38h,1Eh,9Eh,0E0h,0B8h,0
        db  'key 29, 1E        $', 29h,0A9h,1Eh,9Eh,0
        db  'key 29, space     $', 29h,0A9h,39h,0B9h,0
        db  'key 29, 2D        $', 29h,0A9h,2Dh,0ADh,0
        db  'key 29 twice, 12  $', 29h,0A9h,29h,0A9h,12h,92h,0
        db  'key 0D, 12        $', 0Dh,8Dh,12h,92h,0
        db  'shift 0D, 1E      $', 2Ah,0Dh,8Dh,0AAh,1Eh,9Eh,0
        db  'key 1A, shifted   $', 1Ah,9Ah,2Ah,1Ah,9Ah,0AAh,0
        db  'key 1A, 12        $', 1Ah,9Ah,12h,92h,0
        db  'shift 1A, 16      $', 2Ah,1Ah,9Ah,0AAh,16h,96h,0
        db  'key 0C 27 28 2B   $', 0Ch,8Ch,27h,0A7h,28h,0A8h,2Bh,0ABh,0
        db  'shift 0C 27 28 2B $', 2Ah,0Ch,8Ch,27h,0A7h,28h,0A8h,2Bh,0ABh,0AAh,0
        db  'key 10 11 1E 2C 32$', 10h,90h,11h,91h,1Eh,9Eh,2Ch,0ACh,32h,0B2h,0
        db  'key 33 34 35      $', 33h,0B3h,34h,0B4h,35h,0B5h,0
        db  'shift 33 34 35    $', 2Ah,33h,0B3h,34h,0B4h,35h,0B5h,0AAh,0
        db  'key 56, shifted   $', 56h,0D6h,2Ah,56h,0D6h,0AAh,0
        db  'altgr 56          $', 0E0h,38h,56h,0D6h,0E0h,0B8h,0
        db  'caps: 1E 02 27    $', 3Ah,0BAh,1Eh,9Eh,02h,82h,27h,0A7h,3Ah,0BAh,0
        db  'caps, shift 1E 02 $', 3Ah,0BAh,2Ah,1Eh,9Eh,02h,82h,0AAh,3Ah,0BAh,0
        db  'alt 15, alt 2C    $', 38h,15h,95h,0B8h,38h,2Ch,0ACh,0B8h,0
        db  'alt 10, alt 1E    $', 38h,10h,90h,0B8h,38h,1Eh,9Eh,0B8h,0
        db  'alt 02            $', 38h,02h,82h,0B8h,0
        db  'ctrl 15, ctrl 2C  $', 1Dh,15h,95h,9Dh,1Dh,2Ch,0ACh,9Dh,0
        db  'ctrl 10, ctrl 32  $', 1Dh,10h,90h,9Dh,1Dh,32h,0B2h,9Dh,0
        db  'keypad . and 5    $', 53h,0D3h,4Ch,0CCh,0
        db  'cursor left, home $', 0E0h,4Bh,0E0h,0CBh,0E0h,47h,0E0h,0C7h,0
        db  'F1, shift F1      $', 3Bh,0BBh,2Ah,3Bh,0BBh,0AAh,0
        db  'enter esc space   $', 1Ch,9Ch,01h,81h,39h,0B9h,0
        db  'ctrl alt F1, 15   $', 1Dh,38h,3Bh,0BBh,0B8h,9Dh,15h,95h,0
        db  'ctrl alt F2, 15   $', 1Dh,38h,3Ch,0BCh,0B8h,9Dh,15h,95h,0
        db  0

msg_plain   db  'KBLTEST: keys pressed by scan code, and what INT 16h read',13,10,'$'
msg_hooked  db  'with INT 09h hooked, and each key passed on:',13,10,'$'
msg_count   db  'INT 09h calls the handler saw: $'
old9        dd  0
n_int9      dw  0

_KT     ENDS
        END     start
