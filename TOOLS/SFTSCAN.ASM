;=====================================================================
; SFTSCAN.ASM - SFTSCAN.COM, the scan Windows does, done where it can
;               be read
;
; KRNL386 does not ask this DOS where its file table is.  It opens the
; console five times to plant handles, allocates a descriptor with
; base 0, and SCANS memory 64K window by 64K window for the four
; characters of the console's name - and when it finds THREE of them
; with two EQUAL gaps no wider than 100h it takes that gap as the size
; of one SFT entry.  If it never finds such a triple the measurement
; returns FFFFh and the kernel says "Incorrect MS-DOS version" and
; exits, which is a sentence about the DOS being unusable and not
; about its version.  KRNL386, segment 1, BA10h through BB47h.
;
; v0.53 made the entries carry their names so that scan can succeed.
; The evidence that it DID succeed was that Windows stopped opening
; the console ten times and opened it five - and that evidence is
; WORTHLESS, because a fault part way through the first five also
; produces five.  The two readings are indistinguishable from outside.
;
; SO DO THE SCAN HERE.  Same rule, same order, same window stride, on
; the same DOS, with a console to print the answer to.  It says where
; the names are, how far apart they are, and what entry size a scanner
; would deduce - and if it finds nothing then the v0.53 fix does not
; work and everything downstream of it was read wrong.
;
; This file contains no copy of the name it is looking for: the four
; characters are compared as immediates so that the program cannot
; find itself and report a decoy.  That is not fussiness - the first
; version of the fix it is checking put a second copy of the name 99
; bytes from the first, inside the 100h window, in the image of the
; very DOS being measured.
;
; Run it on the parked DOS:   PARKTEST \SFTSCAN.COM
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_SS     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SS, DS:_SS, ES:_SS, SS:_SS
        ORG     0

MAXHIT  EQU     16
WSTEP   EQU     0FFDh                   ; paragraphs - the stride KRNL386
                                        ; uses, which overlaps each window
                                        ; by 48 bytes so that a name lying
                                        ; across a boundary is still seen
TOPPARA EQU     0A000h                  ; stop at 640K: conventional memory
                                        ; is where a DOS keeps its SFT and
                                        ; where this scan can reach

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; The scan.
;=====================================================================
        xor     bx,bx                   ; window base, in paragraphs
sc_win:
        mov     es,bx
        xor     di,di
sc_look:
        mov     cx,0FFF0h
        sub     cx,di
        jbe     sc_next                 ; this window is done
        mov     al,'C'
        cld
        repne   scasb
        jne     sc_next                 ; not in this window at all
        cmp     byte ptr es:[di],'O'
        jne     sc_look
        cmp     byte ptr es:[di+1],'N'
        jne     sc_look
        cmp     byte ptr es:[di+2],' '
        jne     sc_look
        call    rec_hit
        jmp     short sc_look
sc_next:
        add     bx,WSTEP
        cmp     bx,TOPPARA
        jb      sc_win

;=====================================================================
; What was found.
;=====================================================================
        mov     cx,[nhits]
        or      cx,cx
        jnz     sc_some
        mov     dx,OFFSET msg_none
        call    puts
        jmp     sc_verdict
sc_some:
        mov     dx,OFFSET msg_found
        call    puts
        mov     ax,[nhits]
        call    put4
        call    crlf

        xor     esi,esi                   ; index into hits
sc_print:
        mov     cx,[nhits]
        cmp     si,cx
        jae     sc_verdict
        mov     dx,OFFSET msg_ind
        call    puts
        mov     eax,[hits + esi*4]
        call    put8
        or      si,si
        jz      sc_nogap
        mov     dx,OFFSET msg_gap
        call    puts
        mov     eax,[hits + esi*4]
        sub     eax,[hits + esi*4 - 4]
        call    put8
sc_nogap:
        call    crlf
        inc     esi
        jmp     short sc_print

;=====================================================================
; And the rule itself: the first three with two equal gaps, neither
; wider than 100h.  This is the whole of what the kernel decides on.
;=====================================================================
sc_verdict:
        call    crlf
        mov     cx,[nhits]
        cmp     cx,3
        jb      sc_nomeasure
        sub     cx,2                    ; last usable starting index
        xor     esi,esi
sc_try:
        mov     eax,[hits + esi*4 + 4]
        sub     eax,[hits + esi*4]       ; gap 1
        mov     edx,[hits + esi*4 + 8]
        sub     edx,[hits + esi*4 + 4]   ; gap 2
        cmp     eax,edx
        jne     sc_tnext
        or      eax,eax
        jz      sc_tnext                ; two names at the same address is
                                        ; not a table, it is one name seen
                                        ; twice through overlapping windows
        cmp     eax,100h
        ja      sc_tnext
        push    eax
        mov     dx,OFFSET msg_size
        call    puts
        pop     eax
        push    eax
        call    put8
        mov     dx,OFFSET msg_at
        call    puts
        mov     eax,[hits + esi*4]
        call    put8
        call    crlf
        pop     eax
        cmp     eax,SFT_EXPECT
        je      sc_right
        mov     dx,OFFSET msg_wrong
        call    puts
        jmp     short sc_bye
sc_right:
        mov     dx,OFFSET msg_ok
        call    puts
        jmp     short sc_bye
sc_tnext:
        inc     esi
        dec     cx
        jnz     sc_try
sc_nomeasure:
        mov     dx,OFFSET msg_nomeas
        call    puts

sc_bye:
;--- and what the real-mode vectors hold after the park.  The extender
;--- reflects an interrupt by going down to real mode and through the
;--- IVT, so a vector still pointing at PM-DOS s V86 monitor - gone the
;--- moment the kernel left - is a cliff.  Printed on every path out,
;--- because the first version of this was jumped over by all of them.
        mov     dx,OFFSET msg_vecs
        call    puts
        mov     bl,11h
        call    showvec
        mov     bl,12h
        call    showvec
        mov     bl,15h
        call    showvec
        mov     bl,1Ah
        call    showvec
        mov     dx,OFFSET msg_bye
        call    puts
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; rec_hit - a hit at ES:DI-1.  Keeps the linear address, drops one that
;          is not past the last: the windows overlap by 48 bytes on
;          purpose and a name inside that overlap is seen twice.
;---------------------------------------------------------------------
;--- showvec - BL = a vector, printed as its number and contents ------
showvec:
        push    es
        push    ax
        push    bx
        push    di
        mov     dx,OFFSET msg_vec1
        call    puts
        mov     al,bl
        call    put2
        mov     dx,OFFSET msg_vec2
        call    puts
        xor     ax,ax
        mov     es,ax
        movzx   di,bl
        shl     di,2
        mov     ax,es:[di+2]
        call    put4
        mov     dl,3Ah
        call    putc
        mov     ax,es:[di]
        call    put4
        call    crlf
        pop     di
        pop     bx
        pop     ax
        pop     es
        ret

rec_hit:
        pushad
        mov     cx,[nhits]
        cmp     cx,MAXHIT
        jae     rec_done
        movzx   eax,bx
        shl     eax,4
        movzx   edx,di
        dec     edx
        add     eax,edx                 ; EAX = linear address of the 'C'
        or      cx,cx
        jz      rec_keep
        movzx   esi,cx
        dec     esi
        cmp     eax,[hits + esi*4]
        jbe     rec_done                ; already have this one
rec_keep:
        movzx   esi,word ptr [nhits]
        mov     [hits + esi*4],eax
        inc     word ptr [nhits]
rec_done:
        popad
        ret

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
; SFT_EXPECT is RMDOS's SFT_LEN and is spelt out rather than included,
; because this program is a check ON that kernel and a check that gets
; its expected value from the thing it is checking is not a check.
;---------------------------------------------------------------------
SFT_EXPECT EQU  30h

nhits       dw  0
hits        dd  MAXHIT dup (0)

msg_hi      db  13,10,'SFTSCAN - the scan KRNL386 does, where it can be read',13,10
            db  'looking for the console name, three with equal gaps',13,10,13,10,'$'
msg_none    db  'NOT ONE anywhere below 640K.',13,10
            db  'The entries carry no names and this DOS cannot be measured.',13,10,'$'
msg_found   db  'names found: $'
msg_ind     db  '  at $'
msg_gap     db  '  gap $'
msg_size    db  'a scanner would call an entry $'
msg_at      db  ' bytes, table at $'
msg_ok      db  'and that is SFT_LEN - the measurement is RIGHT.',13,10,'$'
msg_wrong   db  'BUT SFT_LEN IS 30h - it would measure the wrong thing.',13,10,'$'
msg_nomeas  db  'no three with equal gaps: the measurement returns FFFFh,',13,10
            db  'which is what makes the kernel reject the DOS.',13,10,'$'
msg_bye     db  13,10,'SFTSCAN done',13,10,'$'
msg_vecs    db  13,10,'real-mode vectors after the park:',13,10,'$'
msg_vec1    db  '  INT ','$'
msg_vec2    db  'h  -> ','$'
msg_crlf    db  13,10,'$'

_SS     ENDS
        END     start
