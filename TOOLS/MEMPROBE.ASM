;=====================================================================
; MEMPROBE.ASM - MEMPROBE.COM, a 16-bit DOS program that asks the BIOS
;                how much memory there is, every way a kernel could
;                ask, and prints the raw answers.
;
;   INT 15h AX=E801h   the registers as returned, and CF
;   INT 15h AH=88h     the same
;   INT 15h E820h      the first entries of the map, if there is one
;   CMOS 15h-18h, 30h-31h, 34h-35h   what the BIOS itself wrote down
;
; Runs under real DOS, where the answers are the BIOS's own, and in a
; PM-DOS box, where they are the kernel's.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_MP     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_MP, DS:_MP, ES:_MP, SS:_MP
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- E801h and 88h, entered with CF clear and then with CF set ---
        mov     byte ptr [entry_cf],0
        call    two_calls
        mov     byte ptr [entry_cf],1
        call    two_calls

;--- E820h -----------------------------------------------------------
        mov     dx,OFFSET msg_e820
        mov     ah,09h
        int     21h
        xor     ebx,ebx
        mov     byte ptr [e820n],0
e820lp:
        push    cs
        pop     es
        mov     di,OFFSET e820buf
        mov     eax,0E820h
        mov     edx,534D4150h
        mov     ecx,20
        int     15h
        jc      e820end
        cmp     eax,534D4150h
        jne     e820end
        push    ebx
        mov     dx,OFFSET msg_e820e
        mov     ah,09h
        int     21h
        mov     eax,dword ptr [e820buf+4]
        call    hex32
        mov     eax,dword ptr [e820buf]
        call    hex32
        call    space
        mov     eax,dword ptr [e820buf+12]
        call    hex32
        mov     eax,dword ptr [e820buf+8]
        call    hex32
        call    space
        mov     eax,dword ptr [e820buf+16]
        call    hex32
        call    crlf
        pop     ebx
        inc     byte ptr [e820n]
        cmp     byte ptr [e820n],12
        jae     e820end
        or      ebx,ebx
        jnz     e820lp
e820end:
        cmp     byte ptr [e820n],0
        jne     e820done
        mov     dx,OFFSET msg_none
        mov     ah,09h
        int     21h
e820done:

;--- CMOS ------------------------------------------------------------
        mov     dx,OFFSET msg_cmos
        mov     ah,09h
        int     21h
        mov     si,OFFSET cmos_list
cmos_lp:
        lodsb
        or      al,al
        jz      cmos_done
        push    ax
        call    hex8
        mov     al,'='
        call    putc
        pop     ax
        cli
        out     70h,al
        jmp     $+2
        in      al,71h
        sti
        call    hex8
        call    space
        jmp     short cmos_lp
cmos_done:
        call    crlf

        mov     ax,4C00h
        int     21h

two_calls:
        mov     dx,OFFSET msg_e801
        mov     ah,09h
        int     21h
        xor     ax,ax
        xor     bx,bx
        xor     cx,cx
        xor     dx,dx
        call    entry_flag
        mov     ax,0E801h
        int     15h
        pushf
        push    dx
        push    cx
        push    bx
        push    ax
        pop     ax
        call    hex16
        call    space
        pop     ax
        call    hex16
        call    space
        pop     ax
        call    hex16
        call    space
        pop     ax
        call    hex16
        popf
        call    say_cf
        mov     dx,OFFSET msg_88
        mov     ah,09h
        int     21h
        xor     ax,ax
        call    entry_flag
        mov     ah,88h
        int     15h
        pushf
        call    hex16
        popf
        call    say_cf
        ret

; entry_flag - CF as [entry_cf] says, and say so
entry_flag:
        cmp     byte ptr [entry_cf],0
        je      ef_clc
        stc
        ret
ef_clc:
        clc
        ret

;---------------------------------------------------------------------
say_cf:
        mov     dx,OFFSET msg_cfset
        jc      sc_say
        mov     dx,OFFSET msg_cfclr
sc_say:
        mov     ah,09h
        int     21h
        ret

hex32:
        push    eax
        shr     eax,16
        call    hex16
        pop     eax
        ; fall into hex16 with the low word
hex16:
        push    ax
        mov     al,ah
        call    hex8
        pop     ax
hex8:
        push    ax
        shr     al,4
        call    nib
        pop     ax
nib:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     putc
        add     al,7
putc:
        push    dx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     dx
        ret
space:
        mov     al,' '
        jmp     short putc
crlf:
        mov     al,13
        call    putc
        mov     al,10
        jmp     short putc

;---------------------------------------------------------------------
msg_hi      db  'MEMPROBE - what the BIOS says about memory',13,10,'$'
msg_e801    db  'INT 15h AX=E801h: AX BX CX DX = $'
msg_88      db  'INT 15h AH=88h:   AX = $'
msg_e820    db  'INT 15h E820h:',13,10,'$'
msg_e820e   db  '  base $'
msg_none    db  '  (none)',13,10,'$'
msg_cfset   db  '  CF=1',13,10,'$'
msg_cfclr   db  '  CF=0',13,10,'$'
msg_cmos    db  'CMOS: $'
cmos_list   db  15h,16h,17h,18h,30h,31h,34h,35h,0
entry_cf    db  0
e820n       db  0
e820buf     db  24 dup(0)

_MP     ENDS
        END     start
