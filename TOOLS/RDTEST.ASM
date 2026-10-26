;=====================================================================
; RDTEST.ASM - RDTEST.COM, RAMDISK.SYS's drive used on the kernel and
;              then on the DOS under Windows
;
; Needs RAMDISK.SYS loaded with its 64K disk (DEVICE=C:\RAMDISK.SYS,
; which RUNTESTS's CONFIG.SYS has).  BLKTEST asks the same questions
; of a real-mode block driver; this one asks them of the first 32-bit
; disk driver, whose sectors the kernel reads by calling it and RMDOS
; by moving the memory itself.  64K AND NOT MORE: with a 512K disk the
; 4MB 386 had 481K of XMS left, and the tests whose native programs
; start native children (EXECLEAK, FPU32, SLOTTEST, VBETEST) failed.
;
;   on the kernel   find the drive - the one 36h says is 64K in 512-byte
;                   clusters - and print its geometry, which is
;                   RAMDISK.SYS's own choice and so a fixed answer; read
;                   its boot sector with INT 25h; write BIG.DAT, 24K of
;                   a pattern, with VERIFY on so that every sector is read
;                   back by the kernel too, and read it again; make SUB
;                   and SUB\SMALL.TXT in it; park with this program's path
;   on RMDOS        find it the same way and print the same geometry;
;                   INT 25h again; BIG.DAT and SUB\SMALL.TXT have to be
;                   what the kernel wrote; write RM.DAT, 16K of another
;                   pattern, and read it back.  The report goes in
;                   C:\RDTEST.RM
;   on the kernel   again: RM.DAT has to be what RMDOS wrote, which
;                   proves its writes reached the disk's memory; then
;                   everything goes and the root has to be empty.  Not
;                   the free clusters: with UNDELETE=ON the deleted
;                   files are kept in \RECYCLED (RECYCLE.INC)
;
; The patterns are words that count, so a sector that lands in the
; wrong place is a mismatch and not a coincidence.
;
; THE PARK RUNS RMDOS IN REAL MODE, where it moves the disk's memory
; by leaving real mode itself.  Under Windows in 386 enhanced mode it
; is inside WIN386's virtual machine and moves it through INT 15h
; AH=87h instead, so the halves can be run one at a time as well:
;
;   RDTEST /K       the kernel's half, before WIN
;   RDTEST /W       RMDOS's half, from WINSTART.BAT or a DOS box under
;                   Windows - with RM.DAT read straight back, since the
;                   kernel will not see it until Windows has ended
;   RDTEST /B       the kernel's last half, after Windows
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_RT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RT, DS:_RT, ES:_RT, SS:_RT
        ORG     0

D_LEN           EQU     31              ; SMALL.TXT's text, checked where it
                                        ; is defined
CHUNK           EQU     8192            ; bytes a write or read
BIG_CHUNKS      EQU     3               ; BIG.DAT: 24K
RM_CHUNKS       EQU     2               ; RM.DAT: 16K
SEED_K          EQU     0A5C3h          ; the kernel's pattern
SEED_R          EQU     05A3Ch          ; and RMDOS's
CLUS_LO         EQU     100             ; a 64K disk in 512-byte clusters,
CLUS_HI         EQU     128             ; and never TESTBLK's 32K
DISK_SECS       EQU     128             ; ...which is this many sectors

start:
        mov     bx,1000h
        mov     ah,4Ah
        int     21h

        mov     word ptr [outh],1
        call    getmode
        cmp     byte ptr [mode],'W'
        je      w_only
        cmp     byte ptr [mode],'K'
        je      k_only
        cmp     byte ptr [mode],'B'
        je      b_only
        mov     ax,0F100h
        xor     bx,bx
        int     21h
        or      bx,bx
        jz      rm_side

;--- the whole of it, starting on the kernel ---------------------------
        mov     dx,OFFSET m_hi
        call    say
        call    part_find
        jc      k_end
        call    part_k

        mov     dx,OFFSET m_park
        call    say
        call    ownpath
        jc      k_nopath
        mov     ax,0F10Bh
        int     21h
        jc      k_noparkd
        mov     [rmexit],al
        call    copyrep

        call    part_b
        jmp     short k_end

;--- one half at a time: /K before Windows, /W inside it, /B after -----
k_only:
        mov     dx,OFFSET m_hi
        call    say
        call    part_find
        jc      k_end
        call    part_k
        jmp     short k_end
b_only:
        mov     dx,OFFSET m_hi
        call    say
        call    part_find
        jc      k_end
        call    part_b
        jmp     short k_end
w_only:
        mov     dx,OFFSET m_whi
        call    say
        call    part_find
        jc      k_end
        call    part_w
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

;--- on the DOS under Windows, parked to with this program's path ------
rm_side:
        mov     dx,OFFSET n_rep
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      rm_nofile
        mov     [outh],ax
        mov     dx,OFFSET m_rmhi
        call    say
        call    part_find
        jc      rm_close
        call    part_w
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
; the halves
;=====================================================================
;--- part_find - the drive, its geometry and its boot sector, on either
;--- DOS.  CF if there is no such drive, and nothing more can be asked.
part_find:
        mov     dx,OFFSET t_find
        call    say
        call    finddrv
        jc      pfd_none
        mov     dl,[drvch]
        call    putc
        mov     dl,':'
        call    putc
        call    ok

        mov     dx,OFFSET t_geom
        call    say
        call    putgeom
        call    ok

        mov     dx,OFFSET t_boot
        call    say
        call    chkboot
        call    okcf

        mov     dx,OFFSET t_edge
        call    say
        call    chkedge
        call    okcf
        clc
        ret
pfd_none:
        call    bad
        stc
        ret

;--- part_k - on the kernel: BIG.DAT with VERIFY on, read back, and
;--- SUB\SMALL.TXT
part_k:
        mov     dx,OFFSET t_kwrite
        call    say
        mov     ax,2E01h                ; VERIFY ON
        int     21h
        mov     si,OFFSET n_big
        mov     ax,SEED_K
        mov     cx,BIG_CHUNKS
        call    putpat
        pushf
        mov     ax,2E00h
        int     21h
        popf
        call    okcf

        mov     dx,OFFSET t_kread
        call    say
        mov     si,OFFSET n_big
        mov     ax,SEED_K
        mov     cx,BIG_CHUNKS
        call    getpat
        call    okcf

        mov     dx,OFFSET t_kdir
        call    say
        mov     dx,OFFSET n_sub
        mov     ah,39h
        int     21h
        jc      pk_dirbad
        mov     si,OFFSET n_small
        mov     di,OFFSET d_small
        mov     cx,D_LEN
        call    putfile
pk_dirbad:
        call    okcf
        ret

;--- part_w - on the other DOS: what the kernel wrote, then RM.DAT
;--- written and read back
part_w:
        mov     dx,OFFSET t_rread
        call    say
        mov     si,OFFSET n_big
        mov     ax,SEED_K
        mov     cx,BIG_CHUNKS
        call    getpat
        call    okcf

        mov     dx,OFFSET t_rsmall
        call    say
        mov     si,OFFSET n_small
        mov     di,OFFSET d_small
        mov     cx,D_LEN
        call    getfile
        call    okcf

        mov     dx,OFFSET t_rwrite
        call    say
        mov     si,OFFSET n_rmdat
        mov     ax,SEED_R
        mov     cx,RM_CHUNKS
        call    putpat
        call    okcf

        mov     dx,OFFSET t_rback
        call    say
        mov     si,OFFSET n_rmdat
        mov     ax,SEED_R
        mov     cx,RM_CHUNKS
        call    getpat
        call    okcf
        ret

;--- part_b - back on the kernel: RM.DAT as the other side wrote it, and
;--- everything away
part_b:
        mov     dx,OFFSET t_kback
        call    say
        mov     si,OFFSET n_rmdat
        mov     ax,SEED_R
        mov     cx,RM_CHUNKS
        call    getpat
        call    okcf

        mov     dx,OFFSET t_kclean
        call    say
        mov     dx,OFFSET n_big         ; everything goes, so the disk is
        mov     ah,41h                  ; as the next test finds it
        int     21h
        mov     dx,OFFSET n_rmdat
        mov     ah,41h
        int     21h
        mov     dx,OFFSET n_small
        mov     ah,41h
        int     21h
        mov     dx,OFFSET n_sub
        mov     ah,3Ah
        int     21h
        mov     dx,OFFSET n_all         ; and no file or directory found
        mov     cx,10h                  ; where they were: what UNDELETE
        mov     ah,4Eh                  ; keeps is in \RECYCLED, which is
        int     21h                     ; hidden, so the free clusters do
        cmc                             ; not come back and are not asked
        call    okcf
        ret

;--- getmode - /K, /W or /B on the command line -> [mode], else 0
getmode:
        mov     byte ptr [mode],0
        movzx   cx,byte ptr ds:[80h]
        mov     si,81h
gm_look:
        jcxz    gm_out
        lodsb
        dec     cx
        cmp     al,'/'
        jne     gm_look
        jcxz    gm_out
        lodsb
        and     al,0DFh                 ; upper case
        mov     [mode],al
gm_out:
        ret

;=====================================================================
; helpers
;=====================================================================
;--- finddrv - the drive 36h says is a 64K disk in 512-byte clusters,
;--- from C: up -> [drvch], [drvdl] and the names, and [geom_*] what
;--- 36h said.  CF if there is none.
finddrv:
        mov     byte ptr [fdd],3        ; C:, 1-based
fd_one:
        mov     dl,[fdd]
        mov     ah,36h
        int     21h
        cmp     ax,0FFFFh
        je      fd_next
        cmp     cx,512
        jne     fd_next
        cmp     ax,1
        jne     fd_next
        cmp     dx,CLUS_LO
        jb      fd_next
        cmp     dx,CLUS_HI
        jbe     fd_got
fd_next:
        inc     byte ptr [fdd]
        cmp     byte ptr [fdd],26
        jbe     fd_one
        stc
        ret
fd_got:
        mov     [geom_spc],ax
        mov     [geom_bps],cx
        mov     [geom_clus],dx
        mov     al,[fdd]
        mov     [drvdl],al
        add     al,'A' - 1
        mov     [drvch],al
        mov     [n_big],al
        mov     [n_rmdat],al
        mov     [n_sub],al
        mov     [n_small],al
        mov     [n_all],al
        clc
        ret

;--- putgeom - what finddrv's 36h said: bytes, sectors, clusters
putgeom:
        mov     ax,[geom_bps]
        call    put4
        mov     dl,' '
        call    putc
        mov     ax,[geom_spc]
        call    put4
        mov     dl,' '
        call    putc
        mov     ax,[geom_clus]
        call    put4
        ret

;--- chkboot - sector 0 through INT 25h: the signature, the label and
;--- the FAT type RAMDISK.SYS wrote there.  CF if any of them is wrong.
chkboot:
        mov     al,[drvdl]
        dec     al                      ; 0 = A:
        mov     cx,1
        xor     dx,dx
        mov     bx,OFFSET secbuf
        int     25h
        pop     dx                      ; the flags INT 25h leaves behind;
        jc      cb_bad                  ; CF is its answer, not theirs
        cmp     word ptr [secbuf+1FEh],0AA55h
        jne     cb_bad
        mov     si,OFFSET secbuf+2Bh
        mov     di,OFFSET b_label
        mov     cx,11 + 8               ; the label, and the type after it
        cld
        repe    cmpsb
        jne     cb_bad
        clc
        ret
cb_bad:
        stc
        ret

;--- chkedge - the disk's last sector reads through INT 25h and the one
;--- after it does not: the end of the unit, held by whichever layer
;--- sees it first.  CF if either answer is wrong.
chkedge:
        mov     al,[drvdl]
        dec     al
        mov     cx,1
        mov     dx,DISK_SECS - 1
        mov     bx,OFFSET secbuf
        int     25h
        pop     dx
        jc      ce_bad
        mov     al,[drvdl]
        dec     al
        mov     cx,1
        mov     dx,DISK_SECS
        mov     bx,OFFSET secbuf
        int     25h
        pop     dx
        jnc     ce_bad                  ; a sector the disk does not have
        clc
        ret
ce_bad:
        stc
        ret

;--- putpat - SI = the name, AX = the seed, CX = how many CHUNKs.  CF
;--- unless every byte was written.
putpat:
        mov     [seed],ax
        mov     [chunks],cx
        mov     word ptr [chunkno],0
        mov     dx,si
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      pp_bad
        mov     [fh],ax
pp_chunk:
        mov     ax,[chunkno]
        cmp     ax,[chunks]
        jae     pp_close
        call    fillpat
        mov     bx,[fh]
        mov     dx,OFFSET buf
        mov     cx,CHUNK
        mov     ah,40h
        int     21h
        jc      pp_closebad
        cmp     ax,CHUNK
        jne     pp_closebad
        inc     word ptr [chunkno]
        jmp     short pp_chunk
pp_close:
        mov     bx,[fh]
        mov     ah,3Eh
        int     21h
        ret                             ; with 3Eh's CF
pp_closebad:
        mov     bx,[fh]
        mov     ah,3Eh
        int     21h
pp_bad:
        stc
        ret

;--- getpat - the same three -> CF unless the file is exactly that
getpat:
        mov     [seed],ax
        mov     [chunks],cx
        mov     word ptr [chunkno],0
        mov     dx,si
        mov     ax,3D00h
        int     21h
        jc      gp_bad
        mov     [fh],ax
gp_chunk:
        mov     bx,[fh]
        mov     dx,OFFSET buf
        mov     cx,CHUNK
        mov     ah,3Fh
        int     21h
        jc      gp_closebad
        mov     cx,[chunkno]
        cmp     cx,[chunks]
        jae     gp_atend                ; past the last: nothing must come
        cmp     ax,CHUNK
        jne     gp_closebad
        call    cmppat
        jc      gp_closebad
        inc     word ptr [chunkno]
        jmp     short gp_chunk
gp_atend:
        or      ax,ax
        jnz     gp_closebad
        mov     bx,[fh]
        mov     ah,3Eh
        int     21h
        ret
gp_closebad:
        mov     bx,[fh]
        mov     ah,3Eh
        int     21h
gp_bad:
        stc
        ret

;--- fillpat / cmppat - the chunk [chunkno] of the pattern [seed]: word
;--- I of chunk K is (K * CHUNK/2 + I) XOR the seed
fillpat:
        mov     di,OFFSET buf
        mov     ax,[chunkno]
        shl     ax,12                   ; CHUNK / 2 words a chunk
        mov     cx,CHUNK / 2
        cld
fp_word:
        mov     dx,ax
        xor     dx,[seed]
        mov     [di],dx
        add     di,2
        inc     ax
        loop    fp_word
        ret
cmppat:
        mov     si,OFFSET buf
        mov     ax,[chunkno]
        shl     ax,12
        mov     cx,CHUNK / 2
cp_word:
        mov     dx,ax
        xor     dx,[seed]
        cmp     [si],dx
        jne     cp_bad
        add     si,2
        inc     ax
        loop    cp_word
        clc
        ret
cp_bad:
        stc
        ret

;--- putfile - SI = the name, DI = the bytes, CX = how many.  CF
;--- unless they were all written.
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
        jc      pf_closebad
        cmp     ax,cx
        jne     pf_closebad
        mov     ah,3Eh
        int     21h
        ret
pf_closebad:
        mov     ah,3Eh
        int     21h
pf_bad:
        stc
        ret

;--- getfile - SI = the name, DI = what it must hold, CX = how long.
;--- CF unless it holds exactly that.
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
        jne     gf_bad
        mov     si,OFFSET buf
        cld
        repe    cmpsb
        jne     gf_bad
        clc
        ret
gf_bad:
        stc
        ret

;--- copyrep - C:\RDTEST.RM onto this side's output, and away
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

okcf:
        jnc     ok
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
m_hi        db  'RDTEST - RAMDISK.SYS, a 32-bit disk driver, as a drive on both DOSes',13,10,'$'
m_rmhi      db  'and on the DOS under Windows, after the handover:',13,10,'$'
m_whi       db  'RDTEST /W - RAMDISK.SYS',39,'s drive, from where this runs',13,10,'$'
m_park      db  'parking the kernel, to use it from there...',13,10,'$'
m_bye       db  'RDTEST done',13,10,'$'
m_ok        db  ' ok',13,10,'$'
m_bad       db  ' FAILED',13,10,'$'
m_crlf      db  13,10,'$'
m_fails     db  'checks failed: $'
m_allok     db  'all checks passed.',13,10,'$'
m_rmexit    db  'the far side ended with $'
m_nopath    db  'FAILED - no path in the environment to park with',13,10,'$'
m_nopark    db  'FAILED - the park was refused, error $'
m_norep     db  'FAILED - the far side left no report',13,10,'$'

t_find      db  '  36h: the 64K drive is $'
t_geom      db  '  36h: bytes a sector, sectors a cluster, clusters $'
t_boot      db  '  25h: its boot sector says RAMDISK, FAT12$'
t_edge      db  '  25h: its last sector reads, and the next is refused$'
t_kwrite    db  '  3Ch/40h: BIG.DAT, 24K, written with VERIFY on$'
t_kread     db  '  3Dh/3Fh: BIG.DAT read back$'
t_kdir      db  '  39h/3Ch: SUB and SUB\SMALL.TXT made$'
t_kback     db  '  back on the kernel, RM.DAT as RMDOS wrote it$'
t_kclean    db  '  41h/3Ah: all of it gone from the root$'
t_rread     db  '  3Dh/3Fh: BIG.DAT, as the kernel wrote it$'
t_rsmall    db  '  3Dh/3Fh: SUB\SMALL.TXT, as the kernel wrote it$'
t_rwrite    db  '  3Ch/40h: RM.DAT, 16K, written on it$'
t_rback     db  '  3Dh/3Fh: RM.DAT read back$'

n_rep       db  'C:\RDTEST.RM',0
n_big       db  '?:\BIG.DAT',0
n_rmdat     db  '?:\RM.DAT',0
n_sub       db  '?:\SUB',0
n_small     db  '?:\SUB\SMALL.TXT',0
n_all       db  '?:\*.*',0
d_small     db  'written by the kernel, in SUB',13,10
IF ($ - d_small) NE D_LEN
        .ERR    <RDTEST: d_small is not D_LEN bytes>
ENDIF
b_label     db  'RAMDISK    ', 'FAT12   '

outh        dw  1
repfh       dw  0
fh          dw  0
geom_bps    dw  0
geom_spc    dw  0
geom_clus   dw  0
seed        dw  0
chunks      dw  0
chunkno     dw  0
drvch       db  0
drvdl       db  0
fdd         db  0
fails       db  0
rmexit      db  0
mode        db  0                       ; 'K', 'W', 'B', or 0 for all of it
onech       db  0
pathbuf     db  128 dup (0)
buf2        db  128 dup (0)
secbuf      db  512 dup (0)
buf         db  CHUNK dup (0)

_RT     ENDS
        END     start
