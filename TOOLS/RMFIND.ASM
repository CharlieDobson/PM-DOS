;=====================================================================
; RMFIND.ASM - RMFIND.COM, what File Manager asks of RMDOS
;
; Like RMTEST.COM this runs with no PM-DOS underneath it: PARKTEST
; parks the kernel and RMDOS.SYS answers every INT 21h.  Run it with
;
;       PARKTEST \RMFIND.COM
;
; It exists because 4Eh and 4Fh went into RMDOS written and never
; executed - the v0.45 notes say so in as many words - and the first
; program ever to call them was Windows 3.1's File Manager, which
; listed an empty directory and could not say why.  A directory walk
; is a bad thing to debug through a graphical shell.  This asks the
; same questions from a program that can print its answers.
;
; The fourth check is the one that matters and it is not about
; enumerating at all.  DOS KEEPS A SEARCH GOING IN THE CALLER'S DTA,
; in the twenty-one bytes in front of the result, and that is not an
; implementation detail programs are unaware of: it is the ONLY reason
; a program can have two searches open at once.  File Manager's
; ReadDirLevel does exactly that - it enumerates a directory, and for
; each subdirectory it finds it recurses and enumerates THAT, and then
; carries on with the outer FindNext.  A DOS that keeps the search in
; one set of globals loses the outer walk the moment the inner one
; starts, and the tree comes up empty with nothing to see.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_RF     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RF, DS:_RF, ES:_RF, SS:_RF
        ORG     0

REFMAX  EQU     64                      ; entries kept for comparison
REFLEN  EQU     14                      ; 13 for the name and a NUL

; Where things sit in a DTA once 4Eh has filled it in.
DTA_ATTR  EQU   15h
DTA_TIME  EQU   16h
DTA_DATE  EQU   18h
DTA_SIZE  EQU   1Ah
DTA_NAME  EQU   1Eh

start:
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     [sp0],sp
        mov     dx,OFFSET m_hi
        call    puts

;--- 1: enumerate the root, and keep the answer -----------------------
; Everything after this compares against what this check collected, so
; a failure here makes the rest meaningless rather than wrong.
c_root:
        mov     word ptr [resume],OFFSET c_attr
        mov     dx,OFFSET t_root
        call    puts
        mov     dx,OFFSET dta1
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET p_root
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jc      c_root_none

        xor     bx,bx
        mov     di,OFFSET reflist
cr_keep:
        cmp     bx,REFMAX
        jae     cr_done
        test    byte ptr [dta1 + DTA_ATTR],10h
        jz      cr_nodir
        inc     byte ptr [sawdir]       ; check 2 reads this
cr_nodir:
        mov     si,OFFSET dta1 + DTA_NAME
        mov     cx,REFLEN
        cld
        rep     movsb                   ; DI walks the table for us
        inc     bx
        mov     ah,4Fh
        int     21h
        jnc     cr_keep
        mov     [endcode],ax            ; how the walk ended
cr_done:
        mov     [refcnt],bx
        mov     ax,bx
        call    put4
        mov     dx,OFFSET m_entries
        call    puts
        mov     ax,[endcode]
        call    put4
        mov     dx,OFFSET m_paren
        call    puts
        ; the first name, so that a count of zero and a count of
        ; rubbish do not look alike
        mov     dx,OFFSET m_firstis
        call    puts
        mov     si,OFFSET reflist
        call    putz
        or      bx,bx
        jz      bad
        call    ok
        jmp     short c_attr
c_root_none:
        call    put4
        mov     dx,OFFSET m_nosearch
        call    puts
        inc     byte ptr [fails]

;--- 2: a plain search returns no directories ------------------------
; The DOS rule is that the attribute in CX ADMITS the awkward kinds
; rather than selecting them, so a search asking for nothing gets
; ordinary files and only ordinary files.
c_attr:
        mov     word ptr [resume],OFFSET c_sub
        mov     dx,OFFSET t_attr
        call    puts
        mov     al,[sawdir]
        call    put2
        mov     dx,OFFSET m_dirs
        call    puts
        cmp     byte ptr [sawdir],0
        jne     bad
        call    ok

;--- 3: a search inside a subdirectory -------------------------------
; A root walk starts at a fixed LBA and a subdirectory walk follows the
; FAT, which is a different path through dir_first and dir_next.  This
; check finds a real subdirectory rather than naming one, so that it
; works on the floppy and on either hard disk.
c_sub:
        mov     word ptr [resume],OFFSET c_nest
        mov     dx,OFFSET t_sub
        call    puts
        mov     dx,OFFSET dta1
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET p_root
        mov     cx,10h
        mov     ah,4Eh
        int     21h
        jc      c_sub_none
cs_look:
        test    byte ptr [dta1 + DTA_ATTR],10h
        jz      cs_next
        cmp     byte ptr [dta1 + DTA_NAME],'.'
        jne     cs_got                  ; '.' and '..' are not somewhere to go
cs_next:
        mov     ah,4Fh
        int     21h
        jnc     cs_look
        jmp     short c_sub_none
cs_got:
        ; "\NAME\*.*"
        mov     di,OFFSET subpath + 1
        mov     si,OFFSET dta1 + DTA_NAME
cs_copy:
        lodsb
        or      al,al
        jz      cs_tail
        stosb
        jmp     short cs_copy
cs_tail:
        mov     si,OFFSET p_tail
        mov     cx,5                    ; '\','*','.','*',0
        rep     movsb
        mov     dx,OFFSET subpath
        call    puts0
        mov     dx,OFFSET m_space
        call    puts

        mov     dx,OFFSET dta1
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET subpath
        mov     cx,10h
        mov     ah,4Eh
        int     21h
        jc      c_sub_bad
        xor     bx,bx
cs_count:
        inc     bx
        mov     ah,4Fh
        int     21h
        jnc     cs_count
        mov     ax,bx
        call    put4
        mov     dx,OFFSET m_entries
        call    puts
        ; every directory has '.' and '..' in it, so anything under two
        ; is a walk that stopped early
        cmp     bx,2
        jb      bad
        call    ok
        jmp     short c_nest
c_sub_none:
        mov     dx,OFFSET m_nosub
        call    puts
        jmp     short c_nest            ; not a failure - some disks have none
c_sub_bad:
        call    put4
        mov     dx,OFFSET m_nosearch
        call    puts
        inc     byte ptr [fails]

;--- 4: two searches at once, which is what File Manager does --------
; Start a walk in one DTA, take two entries from it, then run a whole
; second walk to exhaustion in a DIFFERENT DTA, then point 1Ah back at
; the first one and ask for the next entry.  On DOS that is the third
; entry of the first walk, because the first walk has been sitting in
; those twenty-one bytes the whole time.  Nothing here is exotic: it is
; two ordinary searches that happen to overlap.
c_nest:
        mov     word ptr [resume],OFFSET done
        mov     dx,OFFSET t_nest
        call    puts
        cmp     word ptr [refcnt],3
        jb      cn_short

        mov     dx,OFFSET dta1
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET p_root
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jc      cn_bad
        mov     ah,4Fh
        int     21h                     ; the first walk has given two now
        jc      cn_bad

        mov     dx,OFFSET dta2
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET p_root
        xor     cx,cx
        mov     ah,4Eh
        int     21h
cn_drain:
        jc      cn_drained
        mov     ah,4Fh
        int     21h
        jmp     short cn_drain
cn_drained:
        mov     dx,OFFSET dta1
        mov     ah,1Ah
        int     21h
        mov     ah,4Fh
        int     21h
        jc      cn_lost

        mov     dx,OFFSET m_wanted
        call    puts
        mov     si,OFFSET reflist + REFLEN * 2
        call    putz
        mov     dx,OFFSET m_got
        call    puts
        mov     si,OFFSET dta1 + DTA_NAME
        call    putz
        mov     si,OFFSET reflist + REFLEN * 2
        mov     di,OFFSET dta1 + DTA_NAME
        call    samez
        jc      bad
        call    ok
        jmp     short done
cn_lost:
        push    ax
        mov     dx,OFFSET m_lost
        call    puts
        pop     ax
        call    put4
        mov     dx,OFFSET m_crlf
        call    puts
        inc     byte ptr [fails]
        jmp     short done
cn_short:
        mov     dx,OFFSET m_toofew
        call    puts
        jmp     short done
cn_bad:
        call    put4
        mov     dx,OFFSET m_nosearch
        call    puts
        inc     byte ptr [fails]

done:
        mov     dx,OFFSET m_crlf
        call    puts
        mov     al,[fails]
        or      al,al
        jz      d_allok
        mov     dx,OFFSET m_fails
        call    puts
        mov     al,[fails]
        call    put2
        mov     dx,OFFSET m_crlf
        call    puts
        jmp     short d_bye
d_allok:
        mov     dx,OFFSET m_allok
        call    puts
d_bye:
        mov     dx,OFFSET m_bye
        call    puts
        mov     al,[fails]
        mov     ah,4Ch
        int     21h                     ; ...and RMDOS gives the machine back

; A failed check goes on to the next one rather than unwinding out of
; the run - the same arrangement, and for the same reason, as RMTEST's.
bad:
        mov     dx,OFFSET m_bad
        call    puts
        inc     byte ptr [fails]
        mov     sp,[sp0]
        jmp     word ptr [resume]
ok:
        mov     dx,OFFSET m_ok
        call    puts
        ret

;=====================================================================
; samez - DS:SI and ES:DI, both ASCIZ.  CF set if they differ.
samez:
        push    si
        push    di
        push    ax
sz_next:
        mov     al,[si]
        cmp     al,es:[di]
        jne     sz_no
        or      al,al
        jz      sz_yes
        inc     si
        inc     di
        jmp     short sz_next
sz_yes:
        pop     ax
        pop     di
        pop     si
        clc
        ret
sz_no:
        pop     ax
        pop     di
        pop     si
        stc
        ret

;--- putz - DS:SI, ASCIZ.  09h wants a '$', and a directory entry
;--- does not come with one.
putz:
        push    ax
        push    dx
pz_next:
        mov     dl,[si]
        or      dl,dl
        jz      pz_end
        mov     ah,02h
        int     21h
        inc     si
        jmp     short pz_next
pz_end:
        pop     dx
        pop     ax
        ret
;--- puts0 - the same thing said about DS:DX, so that a path can be
;--- printed without building a second copy of it with a '$' on the end
puts0:
        push    si
        mov     si,dx
        call    putz
        pop     si
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
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1a
        add     al,7
p1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
m_hi        db  13,10,'RMFIND - 4Eh and 4Fh, asked what File Manager asks',13,10,'$'
m_bye       db  'RMFIND done - handing the machine back.',13,10,'$'
m_ok        db  ' ok',13,10,'$'
m_bad       db  ' FAILED',13,10,'$'
m_crlf      db  13,10,'$'
m_fails     db  'checks failed: $'
m_allok     db  'all checks passed.',13,10,'$'
m_entries   db  ' entries $'
m_paren     db  ' at the end$'
m_firstis   db  ', first is $'
m_dirs      db  ' directories in a plain search$'
m_nosearch  db  ' - the search would not start',13,10,'$'
m_nosub     db  'none on this disk - skipped',13,10,'$'
m_space     db  ' $'
m_wanted    db  13,10,'      the third entry should be $'
m_got       db  ' and 4Fh gave $'
m_lost      db  13,10,'      4Fh lost the first search, error $'
m_toofew    db  'too few entries to test - skipped',13,10,'$'

t_root      db  '  4Eh/4Fh the root holds $'
t_attr      db  '  the attribute rule: $'
t_sub       db  '  4Eh in a subdirectory $'
t_nest      db  '  two searches at once$'

p_root      db  '\*.*',0
p_tail      db  '\*.*',0
subpath     db  '\'
            db  16 dup(0)

fails       db  0
sawdir      db  0
resume      dw  0
sp0         dw  0
refcnt      dw  0
endcode     dw  0
reflist     db  REFMAX * REFLEN dup (0)
dta1        db  64 dup (0)
dta2        db  64 dup (0)

_RF     ENDS
        END
