;=====================================================================
; SDSTAT.ASM - SDSTAT.COM: what the SMARTDRV interface answers
;
; A 16-bit DOS program for a DOS box under Windows, where RMDOS and
; its cache (DOS\RMCACHE.INC) are the DOS underneath: INT 2Fh AX=4A10h
; BX=0 for the hit and miss counts, BX=4 for the sizes, and BX=3 DL=0
; for every drive letter's state.  Run against a real SMARTDRV 4.x on
; a real DOS it prints the same lines, which is the point - the two
; can be put side by side.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_SD     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SD, DS:_SD, ES:_SD, SS:_SD
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

        ; Is there one?  CX=EBABh keeps DBLSPACE.BIN quiet, and DS is
        ; saved because some SMARTDRVs are known to lose it here.
        push    ds
        mov     ax,4A10h
        xor     bx,bx
        mov     cx,0EBABh
        int     2Fh
        pop     ds
        cmp     ax,0BABEh
        je      sd_here
        mov     dx,OFFSET msg_none
        call    puts
        jmp     sd_exit
sd_here:
        mov     word ptr [hits],bx
        mov     word ptr [hits+2],dx
        mov     word ptr [misses],si
        mov     word ptr [misses+2],di
        mov     [dirty],cx
        mov     [version],bp

        mov     dx,OFFSET msg_ver
        call    puts
        mov     ax,[version]
        call    put4
        mov     dx,OFFSET msg_hits
        call    puts
        mov     eax,[hits]
        call    putdec
        mov     dx,OFFSET msg_misses
        call    puts
        mov     eax,[misses]
        call    putdec
        mov     dx,OFFSET msg_dirty
        call    puts
        movzx   eax,word ptr [dirty]
        call    putdec
        mov     dx,OFFSET msg_crlf
        call    puts

        ; The sizes
        push    ds
        mov     ax,4A10h
        mov     bx,4
        int     2Fh
        pop     ds
        mov     [elsize],cx
        mov     [elnow],bx
        mov     [eldos],ax
        mov     [elwin],dx
        mov     dx,OFFSET msg_el
        call    puts
        movzx   eax,word ptr [elsize]
        call    putdec
        mov     dx,OFFSET msg_elnow
        call    puts
        movzx   eax,word ptr [elnow]
        call    putdec
        mov     dx,OFFSET msg_eldos
        call    puts
        movzx   eax,word ptr [eldos]
        call    putdec
        mov     dx,OFFSET msg_elwin
        call    puts
        movzx   eax,word ptr [elwin]
        call    putdec
        mov     dx,OFFSET msg_kb
        call    puts
        movzx   eax,word ptr [elnow]
        movzx   ecx,word ptr [elsize]
        imul    eax,ecx
        shr     eax,10
        call    putdec
        mov     dx,OFFSET msg_kbend
        call    puts

        ; The drives: r = read caching, w = write caching, - = not
        mov     dx,OFFSET msg_drives
        call    puts
        xor     bp,bp
sd_drive:
        push    bp
        push    ds
        mov     ax,4A10h
        mov     bx,3
        xor     dl,dl
        int     2Fh
        pop     ds
        pop     bp
        cmp     dl,0FFh
        je      sd_next
        mov     [dstate],dl
        mov     dl,' '
        call    putc
        mov     dx,bp
        add     dl,'A'
        call    putc
        mov     dl,':'
        call    putc
        mov     dl,'r'
        test    byte ptr [dstate],80h
        jz      sd_r
        mov     dl,'-'
sd_r:
        call    putc
        mov     dl,'w'
        test    byte ptr [dstate],40h
        jz      sd_w
        mov     dl,'-'
sd_w:
        call    putc
sd_next:
        inc     bp
        cmp     bp,26
        jb      sd_drive
        mov     dx,OFFSET msg_crlf
        call    puts

sd_exit:
        mov     ax,4C00h
        int     21h

;=====================================================================
; output
;=====================================================================
puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
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
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        ret
putdec:                                 ; EAX in decimal
        pushad
        mov     di,OFFSET decbuf + 11
        mov     byte ptr [di],'$'
        mov     ecx,10
pd_loop:
        xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     di
        mov     [di],dl
        or      eax,eax
        jnz     pd_loop
        mov     dx,di
        call    puts
        popad
        ret

;=====================================================================
; data
;=====================================================================
msg_hi      db  'SDSTAT - the SMARTDRV interface, INT 2Fh 4A10h',13,10,'$'
msg_none    db  'No SMARTDRV-style cache answers.',13,10,'$'
msg_ver     db  'version $'
msg_hits    db  '  hits $'
msg_misses  db  '  misses $'
msg_dirty   db  '  dirty $'
msg_el      db  'element $'
msg_elnow   db  ' bytes; elements now $'
msg_eldos   db  ', under DOS $'
msg_elwin   db  ', under Windows $'
msg_kb      db  ' ($'
msg_kbend   db  'K now)',13,10,'$'
msg_drives  db  'drives:$'
msg_crlf    db  13,10,'$'

hits        dd  0
misses      dd  0
dirty       dw  0
version     dw  0
elsize      dw  0
elnow       dw  0
eldos       dw  0
elwin       dw  0
dstate      db  0
decbuf      db  12 dup(0)

_SD     ENDS
        END
