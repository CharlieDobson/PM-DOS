;=====================================================================
; CDSTEST.ASM - CDSTEST.COM, the current directory array, read through
;               the list of lists as a DOS program reads it, on the
;               kernel and then on the DOS under Windows
;
; The kernel had no such array until 2026-10-06 - the list of lists
; said FFFF:FFFF - and Windows for Workgroups' Setup read its current
; directories out of whatever was there, found SUBST and JOIN in force,
; and stopped to ask about them.  RMDOS had one, in a shape of its own
; that Windows for Workgroups' IFSMGR.386 writes into as if it were
; DOS's.  Both are DOS's shape now: "C:\DIR" at 0, the flags at 43h,
; the root backslash's offset, 2, at 4Fh.  This reads them.
;
; THE SAME CHECKS TWICE, as DRVTEST does it: on the kernel, then on
; RMDOS through a park with its own path, that copy's answers coming
; back in C:\CDSTEST.RM.  On the far side the current directory should
; be the one this side had - RMDOS starts every drive where the kernel
; left it - which before was always the root.
;
; RUNTESTS runs every test in C:\WORK, so that is the directory both
; sides should find, and a CHDIR to \TESTS and back has to show in the
; array too.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CT, DS:_CT, ES:_CT, SS:_CT
        ORG     0

CDS_LEN         EQU     58h
CDS_FLAGS       EQU     43h
CDS_BACK        EQU     4Fh
CDSF_BAD        EQU     0B000h          ; network, JOIN, SUBST

start:
        mov     bx,1000h                ; room for the copy the park loads
        mov     ah,4Ah
        int     21h

        mov     word ptr [outh],1
        mov     ax,0F100h
        xor     bx,bx
        int     21h
        or      bx,bx
        jz      rm_side

;--- on the kernel -----------------------------------------------------
        mov     dx,OFFSET m_hi
        call    say
        call    checks
        mov     al,[fails]
        mov     [kfails],al

        mov     dx,OFFSET m_park
        call    say
        call    ownpath
        jc      k_nopath
        mov     ax,0F10Bh
        int     21h
        jc      k_noparkd
        mov     [rmexit],al

        mov     dx,OFFSET n_rep
        mov     ax,3D00h
        int     21h
        jc      k_norep
        mov     [repfh],ax
k_copy:
        mov     bx,[repfh]
        mov     cx,128
        mov     dx,OFFSET buf2
        mov     ah,3Fh
        int     21h
        jc      k_copied
        or      ax,ax
        jz      k_copied
        mov     cx,ax
        mov     bx,[outh]
        mov     dx,OFFSET buf2
        mov     ah,40h
        int     21h
        jmp     short k_copy
k_copied:
        mov     bx,[repfh]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_rep
        mov     ah,41h
        int     21h
        mov     dx,OFFSET m_rmexit
        call    say
        mov     al,[rmexit]
        call    put2
        call    crlf

        ; AND HOME AGAIN IN THE SAME DIRECTORY.  The unpark remounts
        ; every volume, and a mount starts at the root; the kernel walks
        ; each drive back to where it was (PARK.INC, park_remount).
        mov     byte ptr [fails],0      ; counted here, added to the kernel's
        mov     dx,OFFSET t_home
        call    say
        mov     si,OFFSET buf
        mov     dl,0
        mov     ah,47h
        int     21h
        mov     si,OFFSET buf
        mov     di,OFFSET n_work
hm_cmp:
        mov     al,[si]
        cmp     al,[di]
        jne     hm_out
        inc     si
        inc     di
        or      al,al
        jnz     hm_cmp
hm_out:
        pushf
        mov     dx,OFFSET buf
        call    sayz
        popf
        call    okif
        mov     al,[fails]
        add     [kfails],al
        jmp     short k_end
k_nopath:
        mov     dx,OFFSET m_nopath
        call    say
        inc     byte ptr [kfails]
        jmp     short k_end
k_noparkd:
        mov     dx,OFFSET m_nopark
        call    say
        call    put4
        call    crlf
        inc     byte ptr [kfails]
        jmp     short k_end
k_norep:
        mov     dx,OFFSET m_norep
        call    say
        inc     byte ptr [kfails]
k_end:
        mov     al,[kfails]
        add     al,[rmexit]
        mov     [fails],al
        or      al,al
        jnz     k_somebad
        mov     dx,OFFSET m_allok
        call    say
        jmp     short k_bye
k_somebad:
        mov     dx,OFFSET m_fails
        call    say
        mov     al,[fails]
        call    put2
        call    crlf
k_bye:
        mov     dx,OFFSET m_bye
        call    say
        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;--- on the DOS under Windows ------------------------------------------
rm_side:
        mov     dx,OFFSET n_rep
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      rm_nofile
        mov     [outh],ax
        mov     dx,OFFSET m_rmhi
        call    say
        call    checks
        mov     bx,[outh]
        mov     ah,3Eh
        int     21h
rm_out:
        mov     al,[fails]
        mov     ah,4Ch
        int     21h
rm_nofile:
        mov     byte ptr [fails],0FFh
        jmp     short rm_out

;=====================================================================
; checks
;=====================================================================
checks:
        mov     byte ptr [fails],0

;--- 52h: is there an array, and how many entries --------------------
        mov     dx,OFFSET t_ptr
        call    say
        call    getcds                  ; -> [cdsoff]/[cdsseg], [ncds]
        mov     al,[ncds]
        call    put2
        cmp     word ptr [cdsoff],0FFFFh
        je      cp_bad
        cmp     byte ptr [ncds],3
        jb      cp_bad
        call    ok
        jmp     short cp_out
cp_bad:
        call    bad
        ret                             ; nothing else can be read
cp_out:

;--- every entry with a drive behind it -------------------------------
        mov     dx,OFFSET t_list
        call    say
        xor     cx,cx
        mov     byte ptr [badbits],0
ls_one:
        call    entry                   ; CL = drive -> ES:DI
        mov     ax,word ptr es:[di+CDS_FLAGS]
        or      ax,ax
        jz      ls_next
        test    ax,CDSF_BAD
        jz      ls_good
        mov     byte ptr [badbits],1
ls_good:
        push    ax
        mov     dx,OFFSET m_ind
        call    say
        pop     ax
        call    put4
        mov     dl,' '
        call    putc
        call    putpath                 ; ES:DI, up to its zero
        call    crlf
ls_next:
        inc     cl
        cmp     cl,[ncds]
        jb      ls_one
        push    cs
        pop     es
        mov     dx,OFFSET t_bits
        call    say
        cmp     byte ptr [badbits],0
        call    okif

;--- the current drive's entry, against 47h ---------------------------
        mov     dx,OFFSET t_cur
        call    say
        call    curmatch
        call    okif

;--- 3Bh moves it -------------------------------------------------------
        mov     dx,OFFSET t_cd
        call    say
        mov     dx,OFFSET n_tests
        mov     ah,3Bh
        int     21h
        jc      cd_bad
        call    getcds                  ; the kernel rewrites it on 52h
        call    curmatch
        pushf
        mov     dl,' '
        call    putc
        mov     dx,OFFSET n_back
        mov     ah,3Bh
        int     21h
        popf
        jne     cd_bad
        call    getcds
        call    curmatch
        jne     cd_bad
        call    ok
        jmp     short cd_out
cd_bad:
        call    bad
cd_out:

        mov     dx,OFFSET m_count
        call    say
        mov     al,[fails]
        call    put2
        call    crlf
        ret

;=====================================================================
; helpers
;=====================================================================
;--- getcds - 52h: the array and its count
getcds:
        push    es
        push    bx
        mov     ah,52h
        int     21h
        mov     ax,word ptr es:[bx+16h]
        mov     [cdsoff],ax
        mov     ax,word ptr es:[bx+18h]
        mov     [cdsseg],ax
        mov     al,byte ptr es:[bx+21h]
        mov     [ncds],al
        pop     bx
        pop     es
        ret

;--- entry - CL = a drive -> ES:DI = its entry
entry:
        push    ax
        mov     al,cl
        mov     ah,CDS_LEN
        mul     ah
        add     ax,[cdsoff]
        mov     di,ax
        mov     es,[cdsseg]
        pop     ax
        ret

;--- curmatch - the current drive's entry is "X:\" and then what 47h
;--- says, with its backslash at 2 and the physical bit set.  ZF if so;
;--- prints the entry's path.
curmatch:
        push    cx
        mov     ah,19h
        int     21h
        mov     cl,al
        mov     si,OFFSET buf
        mov     dl,0
        mov     ah,47h
        int     21h
        call    entry
        call    putpath
        push    cs
        pop     ds
        mov     al,'A'
        add     al,cl
        cmp     byte ptr es:[di],al
        jne     cm_out
        cmp     word ptr es:[di+1],'\:'
        jne     cm_out
        cmp     word ptr es:[di+CDS_BACK],2
        jne     cm_out
        test    word ptr es:[di+CDS_FLAGS],4000h
        jz      cm_no
        add     di,3
        mov     si,OFFSET buf
cm_cmp:
        mov     al,[si]
        cmp     al,es:[di]
        jne     cm_out
        inc     si
        inc     di
        or      al,al
        jnz     cm_cmp                  ; ZF is set: both ended together
        jmp     short cm_out
cm_no:
        or      sp,sp                   ; NZ
cm_out:
        push    cs
        pop     es
        pop     cx
        ret

;--- putpath - ES:DI's ASCIIZ, at most 67 characters
putpath:
        push    ax
        push    cx
        push    dx
        push    di
        mov     cx,67
pp_one:
        mov     dl,es:[di]
        or      dl,dl
        jz      pp_out
        call    putc
        inc     di
        loop    pp_one
pp_out:
        pop     di
        pop     dx
        pop     cx
        pop     ax
        ret

;--- ownpath - DS:DX = this program's path, out of the environment
ownpath:
        push    es
        mov     es,word ptr ds:[2Ch]
        xor     di,di
        mov     cx,8000h
        xor     al,al
        cld
op_var:
        cmp     byte ptr es:[di],0
        je      op_end
        repne   scasb
        jmp     short op_var
op_end:
        inc     di
        cmp     word ptr es:[di],1
        jb      op_none
        add     di,2
        mov     si,OFFSET pathbuf
op_copy:
        mov     al,es:[di]
        mov     [si],al
        inc     di
        inc     si
        or      al,al
        jnz     op_copy
        pop     es
        mov     dx,OFFSET pathbuf
        clc
        ret
op_none:
        pop     es
        stc
        ret

;--- sayz - DS:DX an ASCIIZ string
sayz:
        push    ax
        push    dx
        push    si
        mov     si,dx
sz_one:
        mov     dl,[si]
        or      dl,dl
        jz      sz_out
        call    putc
        inc     si
        jmp     short sz_one
sz_out:
        pop     si
        pop     dx
        pop     ax
        ret

okif:
        je      ok
bad:
        push    dx
        mov     dx,OFFSET m_bad
        call    say
        pop     dx
        inc     byte ptr [fails]
        ret
ok:
        push    dx
        mov     dx,OFFSET m_ok
        call    say
        pop     dx
        ret

say:
        push    ax
        push    bx
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di,dx
        mov     al,'$'
        mov     cx,0FFFFh
        cld
        repne   scasb
        mov     cx,di
        sub     cx,dx
        dec     cx
        mov     bx,[outh]
        mov     ah,40h
        int     21h
        pop     es
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
        push    ds
        push    cs
        pop     ds
        mov     [onech],dl
        mov     dx,OFFSET onech
        mov     cx,1
        mov     bx,[outh]
        mov     ah,40h
        int     21h
        pop     ds
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
crlf:
        push    dx
        mov     dx,OFFSET m_crlf
        call    say
        pop     dx
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
        push    dx
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1a
        add     al,7
p1a:
        mov     dl,al
        call    putc
        pop     dx
        ret

;=====================================================================
m_hi        db  'CDSTEST - the current directory array, through 52h',13,10,'$'
m_rmhi      db  'and on the DOS under Windows, after the handover:',13,10,'$'
m_park      db  'parking the kernel, to ask again...',13,10,'$'
m_bye       db  'CDSTEST done',13,10,'$'
m_ok        db  ' ok',13,10,'$'
m_bad       db  ' FAILED',13,10,'$'
m_crlf      db  13,10,'$'
m_ind       db  '    $'
m_count     db  '  checks failed here: $'
m_fails     db  'checks failed: $'
m_allok     db  'all checks passed.',13,10,'$'
m_rmexit    db  'the far side ended with $'
m_nopath    db  'FAILED - no path in the environment to park with',13,10,'$'
m_nopark    db  'FAILED - the park was refused, error $'
m_norep     db  'FAILED - the far side left no report',13,10,'$'

t_ptr       db  '  52h: an array at LoL+16h, entries at LoL+21h $'
t_list      db  '  the drives it has, flags and path:',13,10,'$'
t_bits      db  '  none of them network, JOIN or SUBST$'
t_cur       db  '  the current drive: X:\, 47h after it, backslash at 2 $'
t_cd        db  '  3Bh to \TESTS and back moves it: $'
t_home      db  '  back on the kernel, 47h still says $'
n_work      db  'WORK',0

n_rep       db  'C:\CDSTEST.RM',0
n_tests     db  '\TESTS',0
n_back      db  '\WORK',0

outh        dw  1
repfh       dw  0
cdsoff      dw  0
cdsseg      dw  0
ncds        db  0
badbits     db  0
fails       db  0
kfails      db  0
rmexit      db  0
onech       db  0
buf         db  64 dup (0)
pathbuf     db  128 dup (0)
buf2        db  128 dup (0)

_CT     ENDS
        END     start
