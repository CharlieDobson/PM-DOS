;=====================================================================
; BLKTEST.ASM - BLKTEST.COM, a block driver CONFIG.SYS loaded, used as
;               a drive on the kernel and then on the DOS under Windows
;
; Needs TESTBLK.SYS loaded (DEVICE=C:\TESTS\TESTBLK.SYS): a 32K FAT12
; RAM disk that formats itself.  RUNTESTS's CONFIG.SYS loads it.
;
; THE KERNEL HAS SERVED A DRIVER'S UNITS AS DRIVE LETTERS SINCE v0.40;
; RMDOS, the DOS under Windows, said they were not there until
; 2026-10-06, and a RAM disk made in CONFIG.SYS vanished under Windows.
; So the questions go both ways across the handover, as DRVTEST's do:
;
;   on the kernel   find the drive (the one 36h says is 32K), write
;                   KERNEL.TXT on it, park with this program's path
;   on RMDOS        read KERNEL.TXT back - the kernel's write, through
;                   the driver - and write RMDOS.TXT; the report goes
;                   in C:\BLKTEST.RM, as DRVTEST's does
;   on the kernel   again: RMDOS.TXT has to be there and say what RMDOS
;                   wrote, which proves its writes reached the driver
;                   and not a cache of its own; then both files go
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_BT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_BT, DS:_BT, ES:_BT, SS:_BT
        ORG     0

D_LEN           EQU     41              ; each file's text, checked where it
                                        ; is defined - up here because wasm
                                        ; will not take an equate before it is

start:
        mov     bx,1000h
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
        mov     dx,OFFSET t_find
        call    say
        call    finddrv
        jc      k_nodrv
        mov     dl,[drvch]
        call    putc
        mov     dl,':'
        call    putc
        call    ok

        mov     dx,OFFSET t_kwrite
        call    say
        mov     si,OFFSET n_kern
        mov     di,OFFSET d_kern
        mov     cx,D_LEN
        call    putfile
        call    okif

        mov     dx,OFFSET m_park
        call    say
        call    ownpath
        jc      k_nopath
        mov     ax,0F10Bh
        int     21h
        jc      k_noparkd
        mov     [rmexit],al
        call    copyrep

        mov     dx,OFFSET t_kback
        call    say
        mov     si,OFFSET n_rm
        mov     di,OFFSET d_rm
        mov     cx,D_LEN
        call    getfile
        call    okif

        mov     si,OFFSET n_kern        ; and both go, so the disk is as
        call    delfile                 ; the next test finds it
        mov     si,OFFSET n_rm
        call    delfile
        jmp     short k_end
k_nodrv:
        call    bad
        jmp     short k_end
k_nopath:
        mov     dx,OFFSET m_nopath
        call    say
        inc     byte ptr [fails]
        jmp     short k_end
k_noparkd:
        mov     dx,OFFSET m_nopark
        call    say
        call    put4
        call    crlf
        inc     byte ptr [fails]
k_end:
        mov     al,[fails]
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

        mov     dx,OFFSET t_find
        call    say
        call    finddrv
        jc      rm_nodrv
        mov     dl,[drvch]
        call    putc
        mov     dl,':'
        call    putc
        call    ok

        mov     dx,OFFSET t_rread
        call    say
        mov     si,OFFSET n_kern
        mov     di,OFFSET d_kern
        mov     cx,D_LEN
        call    getfile
        call    okif

        mov     dx,OFFSET t_rwrite
        call    say
        mov     si,OFFSET n_rm
        mov     di,OFFSET d_rm
        mov     cx,D_LEN
        call    putfile
        call    okif
        jmp     short rm_close
rm_nodrv:
        call    bad
rm_close:
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
; helpers
;=====================================================================
;--- finddrv - the drive 36h says holds 32K or less, from D: up.
;--- -> [drvch], CF if there is none.
finddrv:
        mov     byte ptr [fdd],4        ; D:, 1-based
fd_one:
        mov     dl,[fdd]
        mov     ah,36h
        int     21h
        cmp     ax,0FFFFh
        je      fd_next
        cmp     cx,512
        jne     fd_next
        mov     bx,dx                   ; the total clusters
        mul     cx                      ; AX = bytes a cluster
        or      dx,dx
        jnz     fd_next
        mul     bx                      ; DX:AX = the drive
        or      dx,dx
        jnz     fd_next
        cmp     ax,8000h
        ja      fd_next
        jmp     short fd_got
fd_next:
        inc     byte ptr [fdd]
        cmp     byte ptr [fdd],26
        jbe     fd_one
        stc
        ret
fd_got:
        mov     al,[fdd]
        add     al,'A' - 1
        mov     [drvch],al
        mov     [n_kern],al
        mov     [n_rm],al
        clc
        ret

;--- putfile - SI = the name, DI = the bytes, CX = how many.  ZF if
;--- they were all written.
putfile:
        push    cx
        mov     dx,si
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        pop     cx
        jc      pf_bad
        mov     bx,ax
        mov     dx,di
        push    cx
        mov     ah,40h
        int     21h
        pop     cx
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jc      pf_bad
        cmp     ax,cx
        ret
pf_bad:
        or      sp,sp                   ; NZ
        ret

;--- getfile - SI = the name, DI = what it must hold, CX = how long.
;--- ZF if it holds exactly that.
getfile:
        push    cx
        mov     dx,si
        mov     ax,3D00h
        int     21h
        pop     cx
        jc      gf_bad
        mov     bx,ax
        push    cx
        mov     cx,64
        mov     dx,OFFSET buf
        mov     ah,3Fh
        int     21h
        pop     cx
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jc      gf_bad
        cmp     ax,cx
        jne     gf_out
        mov     si,OFFSET buf
        cld
        repe    cmpsb
gf_out:
        ret
gf_bad:
        or      sp,sp
        ret

delfile:
        mov     dx,si
        mov     ah,41h
        int     21h
        ret

;--- copyrep - C:\BLKTEST.RM onto this side's output, and away
copyrep:
        mov     dx,OFFSET n_rep
        mov     ax,3D00h
        int     21h
        jc      cr_none
        mov     [repfh],ax
cr_copy:
        mov     bx,[repfh]
        mov     cx,128
        mov     dx,OFFSET buf2
        mov     ah,3Fh
        int     21h
        jc      cr_done
        or      ax,ax
        jz      cr_done
        mov     cx,ax
        mov     bx,[outh]
        mov     dx,OFFSET buf2
        mov     ah,40h
        int     21h
        jmp     short cr_copy
cr_done:
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
        ret
cr_none:
        mov     dx,OFFSET m_norep
        call    say
        inc     byte ptr [fails]
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
        mov     [onech],dl
        mov     dx,OFFSET onech
        mov     cx,1
        mov     bx,[outh]
        mov     ah,40h
        int     21h
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
m_hi        db  'BLKTEST - a CONFIG.SYS block driver, as a drive on both DOSes',13,10,'$'
m_rmhi      db  'and on the DOS under Windows, after the handover:',13,10,'$'
m_park      db  'parking the kernel, to use it from there...',13,10,'$'
m_bye       db  'BLKTEST done',13,10,'$'
m_ok        db  ' ok',13,10,'$'
m_bad       db  ' FAILED',13,10,'$'
m_crlf      db  13,10,'$'
m_fails     db  'checks failed: $'
m_allok     db  'all checks passed.',13,10,'$'
m_rmexit    db  'the far side ended with $'
m_nopath    db  'FAILED - no path in the environment to park with',13,10,'$'
m_nopark    db  'FAILED - the park was refused, error $'
m_norep     db  'FAILED - the far side left no report',13,10,'$'

t_find      db  '  36h: the 32K drive is $'
t_kwrite    db  '  3Ch/40h: KERNEL.TXT written on it$'
t_rread     db  '  3Dh/3Fh: KERNEL.TXT, as the kernel wrote it$'
t_rwrite    db  '  3Ch/40h: RMDOS.TXT written on it$'
t_kback     db  '  back on the kernel, RMDOS.TXT as RMDOS wrote it$'

n_rep       db  'C:\BLKTEST.RM',0
n_kern      db  '?:\KERNEL.TXT',0
n_rm        db  '?:\RMDOS.TXT',0
d_kern      db  'written by the kernel, read under RMDOS',13,10
IF ($ - d_kern) NE D_LEN
        .ERR    <BLKTEST: d_kern is not D_LEN bytes>
ENDIF
d_rm        db  'written under RMDOS, read by the kernel',13,10
IF ($ - d_rm) NE D_LEN
        .ERR    <BLKTEST: d_rm is not D_LEN bytes>
ENDIF

outh        dw  1
repfh       dw  0
drvch       db  0
fdd         db  0
fails       db  0
rmexit      db  0
onech       db  0
buf         db  64 dup (0)
pathbuf     db  128 dup (0)
buf2        db  128 dup (0)

_BT     ENDS
        END     start
