;=====================================================================
; SEEKWR.ASM - SEEKWR.COM, a write past a file's end and the write of
;              nothing, as MS-DOS has them, on the kernel and on RMDOS
;
; A write at a position past the end of a file makes the file reach it,
; and lands at that position; a write of nothing (40h, CX=0) makes the
; file exactly as long as the position says - its chain of clusters as
; well as its length, longer or shorter.  Windows makes its swap file
; this way: created, seeked to 3MB, written nothing, and then written in
; pages wherever they fall.  RMDOS used to set only the length, and to
; add one cluster for a write past the chain wherever the write was
; (DOS\RMFILE2.INC, file_trunc and file_io), so the swap file had one
; cluster of its 3MB and a page written 16K in landed 8K in.
;
; ON THE KERNEL it makes the checks there, then parks itself, as
; NETTEST does, to make them again on RMDOS, and prints what the copy
; there wrote to C:\SEEKWR.RM.  Each check prints ok or FAILED; the
; file is C:\SEEKWR.TMP.
;
; Linked with:  wlink form raw bin ... order clname CODE offset=0x100
;=====================================================================
        .386
_SW     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SW, DS:_SW, ES:_SW, SS:_SW
        ORG     0

start:
        mov     bx,1000h                ; 64K: room for the park to load
        mov     ah,4Ah                  ; RMDOS, and for the EXEC there
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
        mov     dx,OFFSET m_kernel
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
        mov     dx,OFFSET buf
        mov     ah,3Fh
        int     21h
        jc      k_copied
        or      ax,ax
        jz      k_copied
        mov     cx,ax
        mov     bx,[outh]
        mov     dx,OFFSET buf
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
        jmp     short k_end
k_nopath:
        mov     dx,OFFSET m_nopath
        call    say
        mov     byte ptr [rmexit],1
        jmp     short k_end
k_noparkd:
        mov     dx,OFFSET m_nopark
        call    say
        mov     byte ptr [rmexit],1
        jmp     short k_end
k_norep:
        mov     dx,OFFSET m_norep
        call    say
        mov     byte ptr [rmexit],1
k_end:
        mov     al,[rmexit]
        or      al,[kfails]
        jnz     k_bad
        mov     dx,OFFSET m_allok
        call    say
        jmp     short k_bye
k_bad:
        mov     dx,OFFSET m_somebad
        call    say
k_bye:
        mov     al,[rmexit]
        or      al,[kfails]
        mov     ah,4Ch
        int     21h

;--- on RMDOS: the same checks, into C:\SEEKWR.RM ----------------------
rm_side:
        mov     dx,OFFSET n_rep
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      rm_nofile
        mov     [outh],ax
        mov     dx,OFFSET m_rmdos
        call    say
        call    checks
        mov     bx,[outh]
        mov     ah,3Eh
        int     21h
        mov     al,[fails]
        mov     ah,4Ch
        int     21h
rm_nofile:
        mov     ax,4CFFh
        int     21h

;=====================================================================
; checks
;=====================================================================
checks:
        mov     byte ptr [fails],0
        call    freeclus
        mov     [free0],eax
        mov     [clusbytes],ecx

        mov     dx,OFFSET n_tmp         ; a new file
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jnc     ck_made
        mov     dx,OFFSET m_nocreate
        jmp     fail_out
ck_made:
        mov     [fh],ax

        ; 4 bytes at 100000, past the end of an empty file
        mov     eax,100000
        call    seekto
        mov     dx,OFFSET d_a
        mov     cx,4
        call    wr
        mov     dx,OFFSET m_w100
        call    say
        cmp     ax,4
        call    okif

        mov     dx,OFFSET m_len100
        call    say
        call    filelen
        cmp     eax,100004
        call    okif

        mov     dx,OFFSET m_r100
        call    say
        mov     eax,100000
        mov     di,OFFSET d_a
        call    rdcheck
        call    okif

        ; nothing written at 300000: 300000 long, all of it there
        mov     eax,300000
        call    seekto
        xor     cx,cx
        call    wr
        mov     dx,OFFSET m_len300
        call    say
        call    filelen
        cmp     eax,300000
        call    okif

        mov     dx,OFFSET m_free300
        call    say
        mov     eax,300000
        call    wantfree
        call    okif

        ; 4 bytes at 250000, inside that, and both read back
        mov     eax,250000
        call    seekto
        mov     dx,OFFSET d_b
        mov     cx,4
        call    wr
        mov     dx,OFFSET m_r250
        call    say
        mov     eax,250000
        mov     di,OFFSET d_b
        call    rdcheck
        call    okif
        mov     dx,OFFSET m_r100b
        call    say
        mov     eax,100000
        mov     di,OFFSET d_a
        call    rdcheck
        call    okif

        ; nothing written at 50000: cut short, the rest given back
        mov     eax,50000
        call    seekto
        xor     cx,cx
        call    wr
        mov     dx,OFFSET m_len50
        call    say
        call    filelen
        cmp     eax,50000
        call    okif
        mov     dx,OFFSET m_free50
        call    say
        mov     eax,50000
        call    wantfree
        call    okif

        ; closed, opened again, still 50000; deleted, all back
        mov     bx,[fh]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_tmp
        mov     ax,3D02h
        int     21h
        mov     [fh],ax
        mov     dx,OFFSET m_reopen
        call    say
        call    filelen
        cmp     eax,50000
        call    okif
        mov     bx,[fh]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_tmp
        mov     ah,41h
        int     21h
        mov     dx,OFFSET m_free0
        call    say
        xor     eax,eax
        call    wantfree
        call    okif
        ret
fail_out:
        call    say
        inc     byte ptr [fails]
        ret

;--- seekto (EAX = position), wr (DS:DX, CX -> AX written), filelen
;--- (-> EAX, the position left at the end)
seekto:
        mov     dx,ax
        shr     eax,16
        mov     cx,ax
        mov     ax,4200h
        mov     bx,[fh]
        int     21h
        ret
wr:
        mov     bx,[fh]
        mov     ah,40h
        int     21h
        jnc     wr_ok
        xor     ax,ax
wr_ok:
        ret
filelen:
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        mov     bx,[fh]
        int     21h
        jc      len_bad
        shl     edx,16
        mov     dx,ax
        mov     eax,edx
        ret
len_bad:
        or      eax,-1
        ret

;--- rdcheck - EAX = position, DI -> the 4 bytes expected: ZF if so
rdcheck:
        push    di
        call    seekto
        mov     bx,[fh]
        mov     cx,4
        mov     dx,OFFSET buf
        mov     ah,3Fh
        int     21h
        pop     di
        jc      rc_no
        cmp     ax,4
        jne     rc_no
        mov     eax,dword ptr [buf]
        cmp     eax,dword ptr [di]
        ret
rc_no:
        test    sp,sp
        ret

;--- freeclus - EAX = free clusters on the current drive, ECX = bytes
;--- a cluster
freeclus:
        mov     ah,36h
        xor     dl,dl
        int     21h
        mul     cx                      ; AX x CX = bytes a cluster
        movzx   ecx,ax
        movzx   eax,bx
        ret

;--- wantfree - EAX = what the file is as long as: ZF if the free count
;--- is the starting one less the clusters that length needs
wantfree:
        add     eax,[clusbytes]
        dec     eax
        xor     edx,edx
        div     dword ptr [clusbytes]
        mov     edx,[free0]
        sub     edx,eax
        push    edx
        call    freeclus
        pop     edx
        cmp     eax,edx
        ret

;--- okif - ZF: "ok", else "FAILED" and a fail counted
okif:
        mov     dx,OFFSET m_ok
        je      ok_say
        mov     dx,OFFSET m_failed
        inc     byte ptr [fails]
ok_say:
        jmp     say

;--- ownpath - DS:DX -> this program's own path, from the environment
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

;--- say - DX -> $-text, to [outh]
say:
        push    ax
        push    bx
        push    cx
        push    di
        mov     di,dx
        xor     cx,cx
say_len:
        cmp     byte ptr [di],'$'
        je      say_go
        inc     di
        inc     cx
        jmp     say_len
say_go:
        mov     bx,[outh]
        mov     ah,40h
        int     21h
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret

m_hi        db  'SEEKWR - writes past the end, and the write of nothing',13,10,'$'
m_kernel    db  '--- on the kernel',13,10,'$'
m_rmdos     db  '--- on RMDOS',13,10,'$'
m_park      db  'parking the kernel, to make them there too...',13,10,'$'
m_nopath    db  'FAILED - no path in the environment to park with',13,10,'$'
m_nopark    db  'FAILED - the park was refused',13,10,'$'
m_norep     db  'FAILED - RMDOS left no C:\SEEKWR.RM',13,10,'$'
m_nocreate  db  'FAILED - C:\SEEKWR.TMP cannot be made',13,10,'$'
m_w100      db  '4 bytes at 100000 in an empty file: $'
m_len100    db  '  100004 long: $'
m_r100      db  '  read back from 100000: $'
m_len300    db  'nothing written at 300000, 300000 long: $'
m_free300   db  '  its clusters taken from the free count: $'
m_r250      db  '4 bytes at 250000, read back: $'
m_r100b     db  '  the ones at 100000 still there: $'
m_len50     db  'nothing written at 50000, 50000 long: $'
m_free50    db  '  the clusters past it given back: $'
m_reopen    db  'closed and opened again, still 50000 long: $'
m_free0     db  'deleted, every cluster back: $'
m_ok        db  'ok',13,10,'$'
m_failed    db  'FAILED',13,10,'$'
m_allok     db  'SEEKWR: all ok',13,10,'$'
m_somebad   db  'SEEKWR: something FAILED',13,10,'$'
n_tmp       db  'C:\SEEKWR.TMP',0
n_rep       db  'C:\SEEKWR.RM',0
d_a         db  'AAAA'
d_b         db  'BBBB'
outh        dw  1
fh          dw  0
repfh       dw  0
fails       db  0
kfails      db  0
rmexit      db  0
free0       dd  0
clusbytes   dd  0
buf         db  128 dup (0)
pathbuf     db  128 dup (0)
_SW     ENDS
        END     start
