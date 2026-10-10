;=====================================================================
; DRVTEST.ASM - DRVTEST.COM, a CONFIG.SYS driver asked what DOS asks
;               one, on the kernel and then on the DOS under Windows
;
; Needs IOCDEV.SYS loaded (DEVICE=C:\TESTS\IOCDEV.SYS) - see IOCDEV.ASM
; for what it answers.  RUNTESTS's CONFIG.SYS loads it.
;
; THE SAME CHECKS TWICE.  Once on PM-DOS, in a DOS box; then the program
; parks the kernel - 21h/F1h AL=0Bh, the handover Windows makes - with
; its own path, so that RMDOS.SYS starts a second copy of it on the
; real-mode DOS the kernel leaves behind.  That copy writes what it
; finds into C:\DRVTEST.RM, because RMDOS's console is not the file
; this one's output was sent to; when the machine comes back, this copy
; prints the file and deletes it.  One run, one report, both DOSes.
;
; Which DOS is running is told by 21h AX=F100h: the kernel answers
; PM-DOS's version in BX, and RMDOS answers it as MS-DOS answers a
; function it does not have - AL=0, BX untouched.
;
; WHAT IT IS REALLY TESTING on the far side is the handover: that the
; driver the kernel loaded is in RMDOS's device chain, that a handle
; on it reaches it, and that the addresses DOS gave it at INIT - InDOS
; and the list of lists, which IFSHLP.SYS keeps and uses for as long as
; Windows runs - still mean what they meant.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_DT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DT, DS:_DT, ES:_DT, SS:_DT
        ORG     0

; IOCDEV.SYS's status block - see IOCDEV.ASM
IOS_SIG         EQU     0
IOS_OPENS       EQU     4
IOS_CLOSES      EQU     6
IOS_WRITTEN     EQU     8
IOS_IOLEN       EQU     0Ah
IOS_IOBUF       EQU     0Ch
IOS_INDOS       EQU     1Ch
IOS_LOL         EQU     20h
IOS_LEN         EQU     24h

start:
        ; ROOM FOR THE OTHER COPY.  The park loads this program again, into
        ; the arena, and a .COM owns all of it until it says otherwise.
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
        call    checks
        mov     al,[fails]
        mov     [kfails],al

        mov     dx,OFFSET m_park
        call    say
        call    ownpath                 ; -> DS:DX, ASCIIZ
        jc      k_nopath
        mov     ax,0F10Bh
        int     21h
        jc      k_noparkd
        mov     [rmexit],al

        ; what the other side wrote
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
        jc      rm_nofile               ; nowhere to say anything: the exit
        mov     [outh],ax               ; code will have to
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
; checks - the questions, and a line each.  [fails] counts.
;=====================================================================
checks:
        mov     byte ptr [fails],0

;--- 52h: is it in the chain, behind NUL? ------------------------------
        mov     dx,OFFSET t_chain
        call    say
        mov     ah,52h
        int     21h
        lea     si,[bx+22h]             ; ES:SI = NUL
        mov     cx,64
ch_walk:
        cmp     word ptr es:[si],0FFFFh
        je      ch_none
        les     si,dword ptr es:[si]
        push    cx
        push    si
        lea     di,[si+0Ah]
        mov     si,OFFSET n_devname
        mov     cx,8
        cld
        repe    cmpsb                   ; DS:SI against ES:DI
        pop     si
        pop     cx
        je      ch_found
        loop    ch_walk
ch_none:
        call    bad
        jmp     ch_out
ch_found:
        call    ok
ch_out:
        push    cs
        pop     es

;--- 3Dh: open it by name -----------------------------------------------
; The driver's counts are its own and outlive a handover: on the far
; side they still hold what this side did.  So what is checked is the
; change, from what INT 2Fh DF00h says before the open.
        mov     ax,0DF00h
        int     2Fh
        mov     [opens0],cx
        mov     [closes0],dx
        mov     dx,OFFSET t_open
        call    say
        mov     dx,OFFSET n_dev
        mov     ax,3D02h
        int     21h
        jnc     op_ok
        call    bad
        ret                             ; nothing else can be asked
op_ok:
        mov     [hnd],ax
        call    ok

;--- 4400h: what the handle is -----------------------------------------
        mov     dx,OFFSET t_info
        call    say
        mov     bx,[hnd]
        mov     ax,4400h
        int     21h
        mov     ax,dx
        call    put4
        cmp     dx,0C8C0h               ; the header's high byte, a device
        call    okif                    ; not at its end

;--- 4402h: the status block, and the open it counted ------------------
        mov     dx,OFFSET t_stat
        call    say
        call    getstat
        jc      st_bad
        cmp     dword ptr [stat + IOS_SIG],'DCOI'       ; 'IOCD'
        jne     st_bad
        mov     ax,word ptr [stat + IOS_OPENS]
        sub     ax,[opens0]
        call    put4
        cmp     ax,1
        call    okif
        jmp     short st_out
st_bad:
        call    bad
st_out:
        mov     ax,word ptr [stat + IOS_WRITTEN]        ; this side's starting
        mov     [written0],ax                           ; point for 40h's count

;--- the addresses DOS gave it at INIT, against today's -----------------
        mov     dx,OFFSET t_indos
        call    say
        mov     ah,34h
        int     21h
        mov     ax,es
        push    cs
        pop     es
        mov     dx,word ptr [stat + IOS_INDOS + 2]
        mov     cx,word ptr [stat + IOS_INDOS]
        call    samelin                 ; AX:BX against DX:CX, as addresses
        call    okif

        mov     dx,OFFSET t_lol
        call    say
        mov     ah,52h
        int     21h
        mov     ax,es
        push    cs
        pop     es
        mov     dx,word ptr [stat + IOS_LOL + 2]
        mov     cx,word ptr [stat + IOS_LOL]
        call    samelin
        call    okif

;--- 4402h with eight bytes: the handshake IFSMGR makes ----------------
        mov     dx,OFFSET t_hand
        call    say
        mov     dword ptr [buf],'NGIS'                  ; 'SIGN'
        mov     bx,[hnd]
        mov     cx,8
        mov     dx,OFFSET buf
        mov     ax,4402h
        int     21h
        jc      hs_bad
        cmp     ax,8
        jne     hs_bad
        cmp     dword ptr [buf],'YAKO'                  ; 'OKAY'
        jne     hs_bad
        push    8
        call    dword ptr [buf+4]       ; the entry it handed back
        add     sp,2
        or      ax,ax
        jnz     hs_bad
        cmp     dx,'IO'
        jne     hs_bad
        call    ok
        jmp     short hs_out
hs_bad:
        call    bad
hs_out:

;--- 4403h, and 4402h reading it back -----------------------------------
        mov     dx,OFFSET t_ioout
        call    say
        mov     bx,[hnd]
        mov     cx,6
        mov     dx,OFFSET n_rhello
        mov     ax,4403h
        int     21h
        jc      io_bad
        cmp     ax,6
        jne     io_bad
        call    getstat
        jc      io_bad
        cmp     word ptr [stat + IOS_IOLEN],6
        jne     io_bad
        mov     si,OFFSET n_rhello
        mov     di,OFFSET stat + IOS_IOBUF
        mov     cx,6
        cld
        repe    cmpsb
        jne     io_bad
        call    ok
        jmp     short io_out
io_bad:
        call    bad
io_out:

;--- 40h: written, and a ^Z where cooked writing stops -----------------
        mov     dx,OFFSET t_write
        call    say
        mov     bx,[hnd]
        mov     cx,3
        mov     dx,OFFSET n_abc
        mov     ah,40h
        int     21h
        call    put4
        mov     dl,' '
        call    putc
        mov     bx,[hnd]
        mov     cx,3
        mov     dx,OFFSET n_xzy
        mov     ah,40h
        int     21h
        call    put4
        mov     dl,' '
        call    putc
        call    getstat
        mov     ax,word ptr [stat + IOS_WRITTEN]
        sub     ax,[written0]
        call    put4
        cmp     ax,4                    ; "abc" and the "x" before the ^Z
        call    okif

;--- 3Fh, cooked: a line at a time, and the ^Z --------------------------
        mov     dx,OFFSET t_cooked
        call    say
        call    rdcount
        call    put4
        mov     [r1],ax
        mov     dl,' '
        call    putc
        call    rdcount
        call    put4
        mov     [r2],ax
        mov     dl,' '
        call    putc
        call    rdcount
        call    put4
        cmp     word ptr [r1],6         ; "LINE1",CR
        jne     ck_bad
        cmp     word ptr [r2],6         ; "LINE2",CR
        jne     ck_bad
        cmp     ax,1                    ; and the ^Z, kept
        jne     ck_bad
        call    ok
        jmp     short ck_out
ck_bad:
        call    bad
ck_out:

;--- 4401h raw, then 3Fh: all of it in one go ---------------------------
; A ^Z read in cooked mode leaves the handle at its end, and DOS keeps
; it there until something is WRITTEN to it - nothing at all will do
; (DOS6SRC disk.asm, WRTDEV) - so that comes first.
        mov     dx,OFFSET t_raw
        call    say
        mov     bx,[hnd]
        xor     cx,cx
        mov     ah,40h
        int     21h
        call    rewind
        mov     bx,[hnd]
        mov     ax,4400h
        int     21h
        xor     dh,dh
        or      dl,20h
        mov     ax,4401h
        int     21h
        mov     ax,4400h
        int     21h
        mov     ax,dx
        call    put4
        mov     dl,' '
        call    putc
        call    rdcount
        call    put4
        cmp     ax,13                   ; both lines and the ^Z
        call    okif

;--- 4406h and 4407h: busy once it is all read, ready after a rewind ---
        mov     dx,OFFSET t_status
        call    say
        mov     bx,[hnd]
        mov     ax,4406h
        int     21h
        mov     [r1],ax
        call    put2
        mov     dl,' '
        call    putc
        call    rewind
        mov     bx,[hnd]
        mov     ax,4406h
        int     21h
        mov     [r2],ax
        call    put2
        mov     dl,' '
        call    putc
        mov     bx,[hnd]
        mov     ax,4407h
        int     21h
        call    put2
        cmp     al,0FFh
        jne     ss_bad
        cmp     byte ptr [r1],00h
        jne     ss_bad
        cmp     byte ptr [r2],0FFh
        jne     ss_bad
        call    ok
        jmp     short ss_out
ss_bad:
        call    bad
ss_out:

;--- 440Ch: the generic IOCTL, and one it does not know ----------------
        mov     dx,OFFSET t_gen
        call    say
        mov     dword ptr [buf],0
        mov     bx,[hnd]
        mov     cx,0AE01h
        mov     dx,OFFSET buf
        mov     ax,440Ch
        int     21h
        jc      gn_bad
        cmp     dword ptr [buf],'INEG'                  ; 'GENI'
        jne     gn_bad
        mov     bx,[hnd]
        mov     cx,0AE02h
        mov     dx,OFFSET buf
        mov     ax,440Ch
        int     21h
        jnc     gn_bad                  ; it must fail...
        call    put4
        cmp     ax,16h                  ; ...with "unknown command"
        call    okif
        jmp     short gn_out
gn_bad:
        call    bad
gn_out:

;--- INT 2Fh: the hook it made at INIT ----------------------------------
        mov     dx,OFFSET t_hook
        call    say
        mov     ax,0DF00h
        int     2Fh
        cmp     al,0FFh
        jne     hk_bad
        cmp     bx,'IO'
        jne     hk_bad
        call    ok
        jmp     short hk_out
hk_bad:
        call    bad
hk_out:

;--- 3Eh, and the close it counted --------------------------------------
        mov     dx,OFFSET t_close
        call    say
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h
        mov     ax,0DF00h
        int     2Fh
        mov     ax,dx
        sub     ax,[closes0]
        call    put4
        cmp     ax,1
        call    okif

        mov     dx,OFFSET m_count
        call    say
        mov     al,[fails]
        call    put2
        call    crlf
        ret

;=====================================================================
; helpers
;=====================================================================
;--- getstat - the status block into [stat].  CF if 4402h failed.
getstat:
        mov     bx,[hnd]
        mov     cx,IOS_LEN
        mov     dx,OFFSET stat
        mov     ax,4402h
        int     21h
        jc      gs_out
        cmp     ax,IOS_LEN
        je      gs_out                  ; equal: the carry is clear
        stc
gs_out:
        ret

;--- rewind - 4403h "R": READ starts again
rewind:
        mov     bx,[hnd]
        mov     cx,1
        mov     dx,OFFSET n_rhello
        mov     ax,4403h
        int     21h
        ret

;--- rdcount - 3Fh for 20 bytes -> AX = how many came
rdcount:
        mov     bx,[hnd]
        mov     cx,20
        mov     dx,OFFSET buf
        mov     ah,3Fh
        int     21h
        jnc     rc_out
        mov     ax,0FFFFh
rc_out:
        ret

;--- samelin - AX:BX and DX:CX the same address?  ZF if so.  Prints the
;--- first as seg:off.
samelin:
        push    ax
        call    put4
        push    dx
        mov     dl,':'
        call    putc
        pop     dx
        mov     ax,bx
        call    put4
        pop     ax
        ; linear = seg * 16 + off, for both, in EAX and EDX
        push    bx
        push    cx
        movzx   eax,ax
        shl     eax,4
        movzx   ebx,bx
        add     eax,ebx
        movzx   edx,dx
        shl     edx,4
        movzx   ecx,cx
        add     edx,ecx
        cmp     eax,edx
        pop     cx
        pop     bx
        ret

;--- ownpath - DS:DX = this program's path, out of the environment.  CF
;--- if there is none to be found.
ownpath:
        push    es
        mov     es,word ptr ds:[2Ch]
        xor     di,di
        mov     cx,8000h
        xor     al,al
        cld
op_var:
        cmp     byte ptr es:[di],0
        je      op_end                  ; the empty string ends the block
        repne   scasb
        jmp     short op_var
op_end:
        inc     di
        cmp     word ptr es:[di],1      ; one string follows: the path
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

;--- okif - ZF -> " ok", else " FAILED" and a count
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

;--- say - DS:DX a '$'-terminated string, to [outh]
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

putc:                                   ; DL
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
put4:                                   ; AX
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:                                   ; AL
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
m_hi        db  'DRVTEST - a CONFIG.SYS driver asked what DOS asks one',13,10,'$'
m_rmhi      db  'and on the DOS under Windows, after the handover:',13,10,'$'
m_park      db  'parking the kernel, to ask again...',13,10,'$'
m_bye       db  'DRVTEST done',13,10,'$'
m_ok        db  ' ok',13,10,'$'
m_bad       db  ' FAILED',13,10,'$'
m_crlf      db  13,10,'$'
m_count     db  '  checks failed here: $'
m_fails     db  'checks failed: $'
m_allok     db  'all checks passed.',13,10,'$'
m_rmexit    db  'the far side ended with $'
m_nopath    db  'FAILED - no path in the environment to park with',13,10,'$'
m_nopark    db  'FAILED - the park was refused, error $'
m_norep     db  'FAILED - the far side left no report',13,10,'$'

t_chain     db  '  52h: the IOCDEV device is in the chain behind NUL$'
t_open      db  '  3Dh: opened by name$'
t_info      db  '  4400h: device information $'
t_stat      db  '  4402h: the status block, opens counted $'
t_indos     db  '  34h now, against what it said at INIT $'
t_lol       db  '  52h now, against what it said at INIT $'
t_hand      db  '  4402h: the 8-byte handshake, and the entry it gives$'
t_ioout     db  '  4403h: written, and read back by 4402h$'
t_write     db  '  40h: abc, x^Zy, and what it was given $'
t_cooked    db  '  3Fh cooked: a line, a line, the ^Z $'
t_raw       db  '  4401h raw, then 3Fh: all of it $'
t_status    db  '  4406h done and rewound, 4407h $'
t_gen       db  '  440Ch: GENI, and an unknown one fails $'
t_hook      db  '  INT 2Fh DF00h: the hook from INIT$'
t_close     db  '  3Eh: closes counted $'

n_dev       db  'IOCDEV$',0
n_devname   db  'IOCDEV$ '
n_rep       db  'C:\DRVTEST.RM',0
n_rhello    db  'RHELLO'
n_abc       db  'abc'
n_xzy       db  'x',1Ah,'y'

outh        dw  1
hnd         dw  0
opens0      dw  0
closes0     dw  0
written0    dw  0
repfh       dw  0
r1          dw  0
r2          dw  0
fails       db  0
kfails      db  0
rmexit      db  0
onech       db  0
stat        db  IOS_LEN dup (0)
buf         db  32 dup (0)
pathbuf     db  128 dup (0)
buf2        db  128 dup (0)

_DT     ENDS
        END     start
