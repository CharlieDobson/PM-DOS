;=====================================================================
; VECBACK.ASM - VECBACK.COM: the vectors a DOS program leaves hooked,
;               put back when it ends.
;
; A program the kernel ends - a fault, a HLT with interrupts off, a ^C
; - never unhooks anything, and until now the kernel did not either:
; Wolfenstein's INT 08h was left on its handler in freed memory, and
; the timer went on being delivered there.  A vector a program changed
; is now put back when the program ends, if it points into memory that
; goes with the program (DOS\VDM.INC, vdm_ivtback).  This checks that
; from the parent's side, the way a program would see it.  Each case
; is a child - this program again, with a letter in its tail:
;
;   C  ended by ^C, the default handler's way: the kernel ends it
;   N  ends with 4Ch, still hooked
;   L  leaves 61h on itself, 62h on a block it gave this program and
;      63h on the ROM: 61h comes back, and the other two stay
;   D  a DPMI client that puts 61h on one of its real-mode callbacks
;      and ends from protected mode: the callback goes with the client
;
; C and N hook 00h, 08h, 1Ch and 61h - 1Ch by writing the table, as
; games do, and the other three through 25h.  INT 08h's handler passes
; the tick on.
;
;   VECBACK /F   saves the vectors to VECBACK.DAT, then runs child A,
;                which hooks the same four and stops with a HLT and
;                interrupts off.  That ends the whole box, this program
;                with it, so nothing more is printed.
;   VECBACK /G   the same, with child S, which ends the way Wolfenstein
;                did: a store through a segment of 0 into the kernel's
;                read-only low pages, at 0:11BDh.  An XCHG, which the
;                monitor does not step over, and of the byte already
;                there, so nothing changes wherever it lands.
;   VECBACK /V   run straight after /F or /G: the vectors are what the
;                file says.  The file is deleted.
;
; Every case puts the vectors back itself afterwards, so a kernel that
; fails one leaves nothing hooked for the tests that follow.  The exit
; code is the number of failures.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_VB     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_VB, DS:_VB, ES:_VB, SS:_VB
        ORG     0

STACK_BYTES     EQU     512
KEY_CTRLC       EQU     2E03h           ; scan code and ASCII of Ctrl+C
NVEC            EQU     6               ; the vectors in vecnum
DATLEN          EQU     NVEC * 4 + 1    ; VECBACK.DAT: before[], as it is,
                                        ; and the child's letter
LOW_STORE       EQU     11BDh           ; where G stores, as Wolfenstein did
ROM_SEG         EQU     0F000h          ; where L points 63h
ROM_OFF         EQU     0FFF0h
SELF_MAX        EQU     128             ; this program's path, with its NUL

start:
;--- a letter in the tail: a child case, or /F or /V ---------------------
        mov     si,81h
        mov     cl,byte ptr ds:[80h]
        xor     ch,ch
        jcxz    no_tail
arg_scan:
        mov     al,byte ptr [si]
        cmp     al,' '
        je      arg_skip
        cmp     al,'/'
        je      arg_skip
        cmp     al,'-'
        jne     arg_got
arg_skip:
        inc     si
        loop    arg_scan
no_tail:
        jmp     main_test
arg_got:
        and     al,0DFh
        cmp     al,'C'
        je      child_c
        cmp     al,'N'
        je      child_n
        cmp     al,'L'
        je      child_l
        cmp     al,'D'
        je      child_d
        cmp     al,'A'
        je      child_a
        cmp     al,'S'
        je      child_s
        cmp     al,'F'
        je      main_f
        cmp     al,'G'
        je      main_g
        cmp     al,'V'
        je      main_v
        jmp     main_test

;=====================================================================
; The parent
;=====================================================================
main_test:
        call    shrink
        call    find_self
        mov     dx,OFFSET m_hi
        call    puts
        call    save_vecs

;--- C: ended by ^C ------------------------------------------------------
        mov     al,'C'
        call    run_child
        mov     bx,0100h                ; AH 1: ended by ^C
        mov     dx,OFFSET m_cend
        call    ended
        mov     dx,OFFSET m_cvec
        call    vecsback

;--- N: 4Ch, still hooked ------------------------------------------------
        mov     al,'N'
        call    run_child
        mov     bx,0000h
        mov     dx,OFFSET m_nend
        call    ended
        mov     dx,OFFSET m_nvec
        call    vecsback

;--- L: memory that stays ------------------------------------------------
        call    t_stays

;--- D: a DPMI client's callback -----------------------------------------
        mov     al,'D'
        call    run_child
        mov     bx,0000h
        mov     dx,OFFSET m_dend
        call    ended
        mov     dx,OFFSET m_dvec
        call    vecsback
        jmp     finish

;---------------------------------------------------------------------
; t_stays - case L.  61h is back; 62h is still on the block, whose
;           header now names this program; 63h is still on the ROM.
;---------------------------------------------------------------------
t_stays:
        mov     word ptr [blk_seg],0
        mov     al,'L'
        call    run_child
        mov     bx,0000h
        mov     dx,OFFSET m_lend
        call    ended

        mov     ax,3561h                ; 61h, into the child
        int     21h
        mov     ax,es
        push    cs
        pop     es
        mov     dx,OFFSET m_l61
        cmp     bx,word ptr [before+3*4]
        jne     ts_61bad
        cmp     ax,word ptr [before+3*4+2]
        jne     ts_61bad
        call    pass
        jmp     ts_62
ts_61bad:
        call    fail

ts_62:
        mov     ax,3562h                ; 62h, on the block
        int     21h
        mov     ax,es
        push    cs
        pop     es
        mov     dx,OFFSET m_l62
        or      bx,bx
        jnz     ts_62bad
        cmp     ax,word ptr [before+4*4+2]
        je      ts_62bad                ; put back: wrongly
        mov     cx,ax
        dec     ax
        push    es
        mov     es,ax                   ; its header
        mov     ax,word ptr es:[1]
        pop     es
        mov     bx,cs
        cmp     ax,bx                   ; owned by this program
        jne     ts_62bad
        mov     word ptr [blk_seg],cx   ; ours to free
        call    pass
        jmp     ts_63
ts_62bad:
        call    fail

ts_63:
        mov     ax,3563h                ; 63h, on the ROM
        int     21h
        mov     ax,es
        push    cs
        pop     es
        mov     dx,OFFSET m_l63
        cmp     bx,ROM_OFF
        jne     ts_63bad
        cmp     ax,ROM_SEG
        jne     ts_63bad
        call    pass
        jmp     ts_tidy
ts_63bad:
        call    fail

ts_tidy:
        call    put_vecs
        mov     ax,word ptr [blk_seg]
        or      ax,ax
        jz      ts_out
        mov     es,ax
        mov     ah,49h
        int     21h
        push    cs
        pop     es
ts_out:
        ret

;---------------------------------------------------------------------
; /F and /G - save, then a child that ends the box
;---------------------------------------------------------------------
main_f:
        mov     byte ptr [endkind],'A'
        mov     word ptr [say_saved],OFFSET m_fsaved
        mov     word ptr [say_still],OFFSET m_fstill
        jmp     main_end
main_g:
        mov     byte ptr [endkind],'S'
        mov     word ptr [say_saved],OFFSET m_gsaved
        mov     word ptr [say_still],OFFSET m_gstill
main_end:
        call    shrink
        call    find_self
        call    save_vecs
        mov     dx,OFFSET datname
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      mf_nofile
        mov     bx,ax
        mov     dx,OFFSET before
        mov     cx,DATLEN
        mov     ah,40h
        int     21h
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jc      mf_nofile
        cmp     ax,DATLEN
        jne     mf_nofile
        mov     dx,word ptr [say_saved]
        call    puts
        mov     al,byte ptr [endkind]
        call    run_child
        ; Still here: the child did not end the box.
        mov     dx,word ptr [say_still]
        call    fail
        call    put_vecs
        jmp     finish
mf_nofile:
        mov     dx,OFFSET m_nowrite
        call    fail
        jmp     finish

;---------------------------------------------------------------------
; /V - compare with what /F saved
;---------------------------------------------------------------------
main_v:
        mov     dx,OFFSET datname
        mov     ax,3D00h
        int     21h
        jc      mv_noread
        mov     bx,ax
        mov     dx,OFFSET before
        mov     cx,DATLEN
        mov     ah,3Fh
        int     21h
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jc      mv_noread
        cmp     ax,DATLEN
        jne     mv_noread
        mov     dx,OFFSET datname
        mov     ah,41h
        int     21h
        mov     dx,OFFSET m_vline_a     ; which child ended the box
        cmp     byte ptr [endkind],'A'
        je      mv_check
        mov     dx,OFFSET m_vline_s
        cmp     byte ptr [endkind],'S'
        jne     mv_noread
mv_check:
        call    vecsback
        jmp     finish
mv_noread:
        mov     dx,OFFSET m_noread
        call    fail

finish:
        mov     dx,OFFSET m_done
        call    puts
        mov     ax,word ptr [nfail]
        call    putdec
        mov     dx,OFFSET m_fails
        call    puts
        mov     al,byte ptr [nfail]
        mov     ah,4Ch
        int     21h

;=====================================================================
; The children
;=====================================================================

;--- C: ^C, with the box's own handler, which ends us ---------------------
child_c:
        call    hook4
        call    kb_flush
        mov     ax,KEY_CTRLC
        call    kb_stuff
        mov     ah,08h
        int     21h
        mov     ax,4C09h                ; still here: the case failed
        int     21h

;--- N: 4Ch, still hooked --------------------------------------------------
child_n:
        call    hook4
        mov     ax,4C00h
        int     21h

;--- A: a HLT with interrupts off -----------------------------------------
child_a:
        call    hook4
        cli
        hlt
        mov     ax,4C07h                ; still here: the case failed
        int     21h

;--- S: a store into the kernel's low pages, through a segment of 0 -------
child_s:
        call    hook4
        xor     ax,ax
        mov     ds,ax
        mov     al,byte ptr ds:[LOW_STORE]      ; the byte that is there,
        xchg    al,byte ptr ds:[LOW_STORE]      ; stored back
        push    cs
        pop     ds
        mov     ax,4C06h                ; still here: the case failed
        int     21h

;--- L: one vector on us, one on a block given away, one on the ROM -------
child_l:
        call    shrink                  ; room for the block
        mov     bx,1
        mov     ah,48h
        int     21h
        jc      cl_fail
        mov     cx,ax                   ; the block
        mov     es,ax
        mov     byte ptr es:[0],0CFh    ; an IRET in it
        dec     ax
        mov     es,ax                   ; its header...
        mov     ax,word ptr ds:[16h]
        mov     word ptr es:[1],ax      ; ...says our parent owns it now
        push    cs
        pop     es
        push    ds
        mov     ds,cx
        xor     dx,dx
        mov     ax,2562h
        int     21h
        mov     ax,ROM_SEG
        mov     ds,ax
        mov     dx,ROM_OFF
        mov     ax,2563h
        int     21h
        pop     ds
        mov     dx,OFFSET c_iret
        mov     ax,2561h
        int     21h
        mov     ax,4C00h
        int     21h
cl_fail:
        mov     ax,4C08h
        int     21h

;--- D: a DPMI client with 61h on its callback ------------------------------
; Exit codes 1-4 say which step failed.
child_d:
        call    shrink                  ; room for the host's data
        mov     ax,1687h
        int     2Fh
        or      ax,ax
        jnz     cd_none
        mov     word ptr [dp_entry],di
        mov     word ptr [dp_entry+2],es
        push    cs
        pop     es
        mov     bx,si                   ; the host's paragraphs
        or      bx,bx
        jz      cd_go
        mov     ah,48h
        int     21h
        jc      cd_none
        mov     es,ax
cd_go:
        xor     ax,ax                   ; a 16-bit client
        call    dword ptr [dp_entry]
        jc      cd_entry
        ; Protected mode: DS is this segment's data selector.
        push    ds
        pop     es                      ; ES:EDI -> the register structure
        mov     edi,OFFSET dp_rmcs
        push    ds
        push    cs
        pop     ds                      ; DS:ESI -> the procedure
        mov     esi,OFFSET dp_proc
        mov     ax,0303h
        int     31h                     ; -> CX:DX, a real-mode address
        pop     ds
        jc      cd_cb
        mov     bl,61h
        mov     ax,0201h
        int     31h
        jc      cd_set
        mov     ax,4C00h                ; from protected mode
        int     21h
cd_none:
        mov     ax,4C01h
        int     21h
cd_entry:
        mov     ax,4C02h
        int     21h
cd_cb:
        mov     ax,4C03h
        int     21h
cd_set:
        mov     ax,4C04h
        int     21h
dp_proc:                                ; the callback's procedure: never
        iret                            ; called

;---------------------------------------------------------------------
; hook4 - 00h, 08h and 61h through 25h; 1Ch by writing the table
;---------------------------------------------------------------------
hook4:
        mov     ax,3508h
        int     21h
        mov     word ptr [c_old08],bx
        mov     word ptr [c_old08+2],es
        push    cs
        pop     es
        mov     dx,OFFSET c_iret
        mov     ax,2500h
        int     21h
        mov     dx,OFFSET c_tick
        mov     ax,2508h
        int     21h
        mov     dx,OFFSET c_iret
        mov     ax,2561h
        int     21h
        push    es
        xor     ax,ax
        mov     es,ax
        cli
        mov     word ptr es:[1Ch*4],OFFSET c_iret
        mov     word ptr es:[1Ch*4+2],cs
        sti
        pop     es
        ret

c_tick:
        inc     word ptr cs:[c_ticks]
        jmp     dword ptr cs:[c_old08]
c_iret:
        iret

;=====================================================================
; Helpers
;=====================================================================

; shrink - this program down to its image and a stack, which moves to
;          the top of what is left.  ES = the PSP.
shrink:
        pop     dx                      ; the way back, off the old stack
        mov     bx,OFFSET prog_end + STACK_BYTES + 15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,OFFSET prog_end + STACK_BYTES
        sti
        jmp     dx

; save_vecs - the vectors in vecnum, into before[]
save_vecs:
        xor     si,si
sv_one:
        mov     al,byte ptr [vecnum+si]
        mov     ah,35h
        int     21h
        mov     di,si
        shl     di,2
        mov     word ptr [before+di],bx
        mov     word ptr [before+di+2],es
        inc     si
        cmp     si,NVEC
        jb      sv_one
        push    cs
        pop     es
        ret

; put_vecs - the vectors in vecnum, back to before[]
put_vecs:
        xor     si,si
pv_one:
        mov     di,si
        shl     di,2
        mov     al,byte ptr [vecnum+si]
        push    ds
        lds     dx,dword ptr [before+di]
        mov     ah,25h
        int     21h
        pop     ds
        inc     si
        cmp     si,NVEC
        jb      pv_one
        ret

; same_vecs - CF clear if every vector in vecnum is what before[] says
same_vecs:
        xor     si,si
smv_one:
        mov     al,byte ptr [vecnum+si]
        mov     ah,35h
        int     21h
        mov     di,si
        shl     di,2
        mov     ax,es
        push    cs
        pop     es
        cmp     bx,word ptr [before+di]
        jne     smv_no
        cmp     ax,word ptr [before+di+2]
        jne     smv_no
        inc     si
        cmp     si,NVEC
        jb      smv_one
        clc
        ret
smv_no:
        stc
        ret

; show_vecs - a line for each vector that is not what before[] says
show_vecs:
        xor     si,si
shv_one:
        mov     al,byte ptr [vecnum+si]
        mov     ah,35h
        int     21h
        mov     di,si
        shl     di,2
        mov     ax,es
        push    cs
        pop     es
        cmp     bx,word ptr [before+di]
        jne     shv_say
        cmp     ax,word ptr [before+di+2]
        je      shv_next
shv_say:
        push    ax
        mov     dx,OFFSET m_vec
        call    puts
        mov     al,byte ptr [vecnum+si]
        call    putbyte
        mov     dx,OFFSET m_is
        call    puts
        pop     ax
        call    puthex
        mov     dl,':'
        call    putch
        mov     ax,bx
        call    puthex
        mov     dx,OFFSET m_was
        call    puts
        mov     ax,word ptr [before+di+2]
        call    puthex
        mov     dl,':'
        call    putch
        mov     ax,word ptr [before+di]
        call    puthex
        mov     dx,OFFSET m_crlf
        call    puts
shv_next:
        inc     si
        cmp     si,NVEC
        jb      shv_one
        ret

; ended - after run_child: CF if 4Bh failed, else AX = 4Dh's answer.
;         BX = the answer wanted, DX = the case's line.
ended:
        jc      en_4b
        cmp     ax,bx
        jne     en_ax
        jmp     pass
en_ax:
        jmp     failax
en_4b:
        push    ax
        call    fail
        mov     dx,OFFSET m_4b
        call    puts
        pop     ax
        call    puthex
        mov     dx,OFFSET m_crlf
        jmp     puts

; vecsback - DX = the line: every vector in vecnum is as before[] says.
;            Each one is put back to that afterwards, either way.
vecsback:
        push    dx
        call    same_vecs
        pop     dx
        jc      vb_bad
        call    pass
        jmp     put_vecs
vb_bad:
        call    fail
        call    show_vecs
        jmp     put_vecs

; run_child - AL = the case letter.  CF if 4Bh failed (AX its error);
;             else AX = what 4Dh says.
run_child:
        mov     byte ptr [ctail+3],al
        mov     word ptr [pb_env],0
        mov     word ptr [pb_tail],OFFSET ctail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs
        mov     dx,OFFSET self
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        push    cs
        pop     ds
        push    cs
        pop     es
        jc      rc_out
        mov     ah,4Dh
        int     21h
        clc
rc_out:
        ret

; find_self - this program's own path, out of the end of its
;             environment, into self
find_self:
        push    ds
        mov     ax,word ptr ds:[2Ch]
        mov     ds,ax
        xor     si,si
fs_var:
        cmp     byte ptr [si],0
        je      fs_end
fs_skip:
        lodsb
        or      al,al
        jnz     fs_skip
        jmp     fs_var
fs_end:
        add     si,3                    ; the NUL and the string count
        mov     di,OFFSET self
        mov     cx,SELF_MAX - 1         ; and no further than self holds
fs_copy:
        lodsb
        mov     byte ptr cs:[di],al
        inc     di
        or      al,al
        jz      fs_done
        loop    fs_copy
        mov     byte ptr cs:[di],0
fs_done:
        pop     ds
        ret

; kb_flush - empty the BIOS keyboard buffer
kb_flush:
        mov     ah,01h
        int     16h
        jz      kf_out
        mov     ah,00h
        int     16h
        jmp     kb_flush
kf_out:
        ret

; kb_stuff - AX = a key, into the BIOS keyboard buffer
kb_stuff:
        mov     cx,ax
        mov     ah,05h
        int     16h
        ret

; failax - fail, and say what 4Dh gave back: AX
failax:
        push    ax
        call    fail
        mov     dx,OFFSET m_gave
        call    puts
        pop     ax
        call    puthex
        mov     dx,OFFSET m_crlf
        call    puts
        ret

; pass / fail - DX = the case's line; a line either way
pass:
        push    dx
        mov     dx,OFFSET m_ok
        call    puts
        pop     dx
        call    puts
        ret
fail:
        push    dx
        mov     dx,OFFSET m_fail
        call    puts
        pop     dx
        call    puts
        inc     word ptr [nfail]
        ret

; puts - the ASCIIZ at DX, to standard output
puts:
        push    ax
        push    bx
        push    cx
        push    si
        mov     si,dx
        xor     cx,cx
pu_len:
        cmp     byte ptr [si],0
        je      pu_go
        inc     si
        inc     cx
        jmp     pu_len
pu_go:
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

; putch - DL, to standard output
putch:
        push    ax
        mov     ah,02h
        int     21h
        pop     ax
        ret

; putbyte - AL as two hex digits
putbyte:
        push    ax
        mov     ah,al
        xor     al,al
        push    cx
        push    dx
        mov     cx,2
        jmp     ph_one

; puthex - AX as four hex digits
puthex:
        push    ax
        push    cx
        push    dx
        mov     cx,4
ph_one:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph_dig
        add     al,7
ph_dig:
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     ax
        loop    ph_one
        pop     dx
        pop     cx
        pop     ax
        ret

; putdec - AX in decimal
putdec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,10
        xor     cx,cx
pd_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     pd_div
pd_out:
        pop     dx
        add     dl,'0'
        mov     ah,02h
        int     21h
        loop    pd_out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; Data
;=====================================================================
m_hi        db  'VECBACK: vectors a child leaves hooked, put back when the child ends',13,10,0
m_ok        db  '  ok    ',0
m_fail      db  '  FAIL  ',0
m_cend      db  '^C: the default handler ends the child, 4Dh says ^C',13,10,0
m_cvec      db  '^C: 00h, 08h, 1Ch and 61h are back as they were',13,10,0
m_nend      db  '4Ch still hooked: the child ends normally',13,10,0
m_nvec      db  '4Ch still hooked: 00h, 08h, 1Ch and 61h are back as they were',13,10,0
m_lend      db  'memory that stays: the child ends normally',13,10,0
m_l61       db  'memory that stays: 61h, on the child, is back',13,10,0
m_l62       db  'memory that stays: 62h stays on the block the child gave its parent',13,10,0
m_l63       db  'memory that stays: 63h stays on the ROM',13,10,0
m_dend      db  'DPMI: the client ends with 4Ch from protected mode',13,10,0
m_dvec      db  'DPMI: 61h, on the client',39,'s real-mode callback, is back',13,10,0
m_fsaved    db  'VECBACK /F: the vectors are saved; the child stops with interrupts off',13,10,0
m_fstill    db  'the child',39,'s HLT with interrupts off did not end the box',13,10,0
m_gsaved    db  'VECBACK /G: the vectors are saved; the child stores into the kernel',39,'s pages',13,10,0
m_gstill    db  'the child',39,'s store into the kernel',39,'s pages did not end the box',13,10,0
m_vline_a   db  'HLT with interrupts off: 00h, 08h, 1Ch and 61h are back as /F saved',13,10,0
m_vline_s   db  'a fault on a store: 00h, 08h, 1Ch and 61h are back as /G saved',13,10,0
m_nowrite   db  'VECBACK.DAT could not be written',13,10,0
m_noread    db  'VECBACK.DAT could not be read: run VECBACK /F first',13,10,0
m_4b        db  '        4Bh failed: ',0
m_gave      db  '        4Dh gave ',0
m_vec       db  '        vector ',0
m_is        db  'h is ',0
m_was       db  ', was ',0
m_crlf      db  13,10,0
m_done      db  'VECBACK: ',0
m_fails     db  ' failed',13,10,0
datname     db  'VECBACK.DAT',0

vecnum      db  00h, 08h, 1Ch, 61h, 62h, 63h
            db  0, 0                    ; (before[] on a dword)
before      dd  NVEC dup(0)
endkind     db  0                       ; ...and the letter after it, as
            db  0, 0, 0                 ; VECBACK.DAT has them
say_saved   dw  0
say_still   dw  0
c_old08     dd  0
c_ticks     dw  0
dp_entry    dd  0
nfail       dw  0
blk_seg     dw  0
ctail       db  3,' /X',13
pb_env      dw  0
pb_tail     dd  0
pb_fcb1     dd  0
pb_fcb2     dd  0
self        db  SELF_MAX dup(0)
dp_rmcs     db  32h dup(0)              ; the callback's register structure
prog_end    LABEL BYTE

_VB     ENDS
        END     start
