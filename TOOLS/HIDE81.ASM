; HIDE81.COM - hide one BIOS hard disk from later enumerators by
; decrementing the BDA hard-disk count at 0040:0075.  Under the WfW
; bridge the USB stick is INT 13h 81h, but PM-DOS owns its UHCI, so
; SeaBIOS can no longer READ 81h; WfW's enhanced-mode init reads 81h's
; boot sector, gets zeroes, and divides by the zero geometry - a
; divide-error storm that hangs the splash.  Taking 81h out of the BDA
; count stops WfW enumerating it; PM-DOS still serves the stick as D:
; through the bridge (which never uses INT 13h 81h).  Diagnostic/proof
; of the root cause; reports old/new count to E9h.  Pure 16-bit .COM.
        .386
_TEXT   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  cs:_TEXT, ds:_TEXT, es:_TEXT, ss:_TEXT
        ORG     100h
start:
        mov     ax, 40h
        mov     es, ax
        mov     si, OFFSET msg
        call    puts
        mov     al, es:[75h]            ; BDA hard-disk count
        call    hexb
        mov     al, '-'
        out     0E9h, al
        mov     al, es:[75h]
        or      al, al
        jz      done                    ; nothing to hide
        dec     al
        mov     es:[75h], al
        call    hexb
done:
        mov     al, 13
        out     0E9h, al
        mov     al, 10
        out     0E9h, al
        mov     ax, 4C00h
        int     21h

hexb:
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    hnib
        mov     al, cl
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
puts:
        lodsb
        or      al, al
        jz      pdone
        out     0E9h, al
        jmp     puts
pdone:
        ret
msg     db      'HIDE81 bda475 ', 0
_TEXT   ENDS
        END     start
