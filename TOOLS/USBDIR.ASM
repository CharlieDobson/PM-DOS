;=====================================================================
; USBDIR.COM - a controllable probe of the USB disk under WfW.
;
; Run in the system VM from WINSTART.BAT (winbuild -Test), so every
; INT 21h it makes is answered by the parked RMDOS, and a USB drive's
; call travels the bridge.  It reports on port E9h directly (one OUT a
; character), which WfW passes straight through - unlike "> PRN", which
; the printer driver spools and never flushes on a headless run.  So
; the line it prints is proof independent of the screen and the spooler.
;
; It walks the one question that matters for BC2: can D:\ be listed?
;   - 19h  which drive is current
;   - 1Ch  D:'s media/geometry (a drive-level mount, no directory)
;   - 47h  D:'s current directory text
;   - 4Eh/4Fh  the handle find-first/next of D:\*.*
;   - 11h/12h  the FCB find-first/next of D:\*.*  (what COMMAND's DIR uses)
; wasm -q usbdir.asm ; wlink form raw bin name USBDIR.COM file usbdir.obj order clname CODE offset=0x100
;=====================================================================
        .386
_T      segment use16 public 'CODE'
        assume  cs:_T, ds:_T, es:_T, ss:_T
        org     100h
start:
        mov     ax,cs
        mov     ds,ax
        mov     es,ax

        mov     si,OFFSET m_begin
        call    e9str

        ; --- 19h current drive ---
        mov     ah,19h
        int     21h
        push    ax
        mov     si,OFFSET m_cur
        call    e9str
        pop     ax
        add     al,'A'
        call    e9ch
        call    e9nl

        ; --- 1Ch media of D: (DL=4) - drive-level, mounts but no dir ---
        mov     si,OFFSET m_1c
        call    e9str
        mov     ah,1Ch
        mov     dl,4                    ; D:
        push    ds                      ; 1Ch returns DS:BX at the media byte,
        int     21h                     ; so it clobbers DS - put ours back
        pop     ds                      ; before the next DS:SI string print
        ; AL = sectors/cluster, FFh = no such drive
        call    e9hexal
        call    e9nl

        ; --- 47h current directory of D: (DL=4, DS:SI -> 64-byte buf) ---
        mov     si,OFFSET m_47
        call    e9str
        mov     ah,47h
        mov     dl,4
        mov     si,OFFSET cdbuf
        int     21h
        jc      d47_fail
        mov     si,OFFSET m_47ok        ; prints "D:\" + the text
        call    e9str
        mov     si,OFFSET cdbuf
        call    e9str
        call    e9nl
        jmp     short do_4e
d47_fail:
        mov     si,OFFSET m_fail
        call    e9str
        call    e9hexax
        call    e9nl

        ; --- 4Eh handle find-first of D:\*.* ---
do_4e:
        mov     si,OFFSET m_4e
        call    e9str
        mov     ah,1Ah                  ; DTA -> our buffer
        mov     dx,OFFSET dta
        int     21h
        mov     ah,4Eh
        mov     cx,0010h                ; normal + subdirectories
        mov     dx,OFFSET spec
        int     21h
        jc      d4e_fail
        mov     si,OFFSET m_ok
        call    e9str
        mov     si,OFFSET dta + 1Eh     ; the found name, ASCIIZ
        call    e9str
        call    e9nl
        mov     cx,6                    ; a few more, to prove 4Fh walks
d4e_next:
        push    cx
        mov     ah,4Fh
        int     21h
        pop     cx
        jc      do_11
        push    cx
        mov     si,OFFSET m_nx
        call    e9str
        mov     si,OFFSET dta + 1Eh
        call    e9str
        call    e9nl
        pop     cx
        loop    d4e_next
        jmp     short do_11
d4e_fail:
        mov     si,OFFSET m_fail
        call    e9str
        call    e9hexax
        call    e9nl

        ; --- 11h FCB find-first of D:\*.* (what COMMAND's DIR uses) ---
do_11:
        mov     si,OFFSET m_11
        call    e9str
        mov     ah,1Ah                  ; DTA -> our buffer
        mov     dx,OFFSET dta
        int     21h
        mov     ah,11h
        mov     dx,OFFSET fcb
        int     21h
        or      al,al
        jnz     d11_fail
        mov     si,OFFSET m_ok
        call    e9str
        mov     si,OFFSET dta + 8       ; the FCB result: name at DTA+8
        mov     cx,11
        call    e9strn
        call    e9nl
        jmp     short d_end
d11_fail:
        mov     si,OFFSET m_fail11
        call    e9str
        call    e9nl

d_end:
        mov     si,OFFSET m_end
        call    e9str
        mov     ax,4C00h
        int     21h

;--- E9 helpers ------------------------------------------------------
e9ch:                                   ; AL -> E9h
        out     0E9h,al
        ret
e9nl:
        push    ax
        mov     al,10
        out     0E9h,al
        pop     ax
        ret
e9str:                                  ; DS:SI = ASCIIZ -> E9h
        push    ax
        push    si
e9s1:
        lodsb
        or      al,al
        jz      e9s2
        out     0E9h,al
        jmp     short e9s1
e9s2:
        pop     si
        pop     ax
        ret
e9strn:                                 ; DS:SI, CX bytes -> E9h
        push    ax
        push    cx
        push    si
e9sn1:
        lodsb
        out     0E9h,al
        loop    e9sn1
        pop     si
        pop     cx
        pop     ax
        ret
e9hexax:                                ; AX as four hex digits
        push    ax
        push    cx
        mov     cx,4
e9h1:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9h2
        add     al,7
e9h2:
        out     0E9h,al
        pop     ax
        loop    e9h1
        pop     cx
        pop     ax
        ret
e9hexal:                                ; AL as two hex digits
        push    ax
        mov     ah,al
        shr     al,4
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9a1
        add     al,7
e9a1:
        out     0E9h,al
        mov     al,ah
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9a2
        add     al,7
e9a2:
        out     0E9h,al
        pop     ax
        ret

m_begin db  10,'>>USBDIR begin',10,0
m_cur   db  '>19 cur=',0
m_1c    db  '>1C D: spc=',0
m_47    db  '>47 D: ',0
m_47ok  db  'cwd=[D:\',0
m_4e    db  '>4E D:\*.* ',0
m_ok    db  'OK nm=',0
m_nx    db  '>4F nm=',0
m_11    db  '>11 D:\*.* ',0
m_fail  db  'FAIL ax=',0
m_fail11 db 'FAIL (al=FF)',0
m_end   db  10,'>>USBDIR end',10,0
spec    db  'D:\*.*',0
fcb     db  4                           ; drive D (1=A)
        db  '???????????'               ; *.* as 11 ?'s
        db  25 dup(0)                    ; rest of the FCB
cdbuf   db  80 dup(0)
dta     db  128 dup(0)
_T      ends
        end     start
