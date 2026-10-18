; INT13GEO.COM - report what INT 13h tells the BIOS caller about drive 81h
; (the USB stick, as SeaBIOS exposes it), to port E9h.  This is the view
; WfW's enhanced-mode init snapshots when it builds its drive table; a
; zero geometry field here is what makes its LBA->CHS divide by zero.
; Pure 16-bit .COM; outputs hex to E9h (QEMU -debugcon), no DOS output.
;
; Build: wasm -q int13geo.asm
;        wlink form raw bin name INT13GEO.COM file int13geo.obj order clname CODE offset=0x100
        .386
_TEXT   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  cs:_TEXT, ds:_TEXT, es:_TEXT, ss:_TEXT
        ORG     100h
start:
        ; --- AH=08h get drive parameters, DL=81h ---
        mov     si, OFFSET mG
        call    puts
        mov     ah, 08h
        mov     dl, 81h
        push    ds
        int     13h
        pop     ds
        pushf
        pop     ax
        and     al, 1                   ; CF
        call    hexb
        mov     al, ' '
        call    e9
        mov     al, ch                  ; cyl low 8
        call    hexb
        mov     al, cl                  ; bits7-6 cyl high, 5-0 sectors
        call    hexb
        mov     al, ' '
        call    e9
        mov     al, dh                  ; max head (heads-1)
        call    hexb
        mov     al, dl                  ; number of drives
        call    hexb
        call    crlf

        ; --- AH=02h read 1 sector CHS (cyl0,head0,sector1), DL=81h ---
        mov     si, OFFSET mR
        call    puts
        mov     bx, OFFSET buf
        mov     ax, 0201h               ; AH=02 read, AL=1 sector
        mov     cx, 0001h               ; cyl0, sector1
        xor     dh, dh                  ; head0
        mov     dl, 81h
        push    ds
        int     13h
        pop     ds
        pushf
        pop     bx
        push    ax                      ; AH = status
        mov     ax, bx
        and     al, 1                   ; CF
        call    hexb
        mov     al, ' '
        call    e9
        pop     ax
        mov     al, ah                  ; status
        call    hexb
        mov     al, ' '
        call    e9
        mov     al, [buf]               ; first two bytes read
        call    hexb
        mov     al, [buf+1]
        call    hexb
        call    crlf

        ; --- AH=42h LBA extended read, LBA 0, DL=81h ---
        mov     si, OFFSET mL
        call    puts
        mov     word ptr [dap+0], 0010h ; size 16, reserved 0
        mov     word ptr [dap+2], 0001h ; 1 sector
        mov     word ptr [dap+4], OFFSET buf
        mov     word ptr [dap+6], cs
        mov     dword ptr [dap+8], 0    ; LBA low
        mov     dword ptr [dap+12], 0   ; LBA high
        mov     ah, 42h
        mov     dl, 81h
        mov     si, OFFSET dap
        push    ds
        int     13h
        pop     ds
        pushf
        pop     bx
        push    ax
        mov     ax, bx
        and     al, 1
        call    hexb
        mov     al, ' '
        call    e9
        pop     ax
        mov     al, ah
        call    hexb
        mov     al, ' '
        call    e9
        mov     al, [buf]
        call    hexb
        mov     al, [buf+1]
        call    hexb
        call    crlf

        mov     ax, 4C00h
        int     21h

; --- helpers: AL byte -> two hex digits to E9h ---
hexb:
        push    ax
        push    cx
        mov      cl, al
        shr     al, 4
        call    hnib
        mov     al, cl
        and     al, 0Fh
        call    hnib
        pop     cx
        pop     ax
        ret
hnib:
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     hn1
        add     al, 7
hn1:
        out     0E9h, al
        ret
e9:
        out     0E9h, al
        ret
crlf:
        mov     al, 13
        out     0E9h, al
        mov     al, 10
        out     0E9h, al
        ret
puts:
        lodsb
        or      al, al
        jz      pdone
        out     0E9h, al
        jmp     puts
pdone:
        ret

mG      db      'INT13 81h AH08 cf cxcx dh dl: ', 0
mR      db      'INT13 81h AH02chs cf st d0d1: ', 0
mL      db      'INT13 81h AH42lba cf st d0d1: ', 0
dap     db      16 dup(0)
buf     db      512 dup(0)
_TEXT   ENDS
        END     start
