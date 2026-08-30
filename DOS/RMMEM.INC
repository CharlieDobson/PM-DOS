;=====================================================================
; RMMEM.INC - the arena, the process, EXEC and terminate
;
; THE ARENA IS NOT BUILT HERE.  It is already in conventional memory,
; in DOS's own format, with the running program in it - PM-DOS's
; lm_* layer put it there because a 16-bit program has to be able to
; walk an MCB chain, and below 1MB the page map is the identity so
; every one of those segment numbers still means what it meant before
; the mode switch.  RMDOS adopts the chain and carries on allocating
; out of it.
;
; That is the single largest reason this file is short.  A DOS that
; had to build an arena would also have to decide where the running
; program went, and there is no answer to that which does not involve
; moving it.
;=====================================================================

;--- MCB fields are RMCB_* in PMDOS.INC, because PM-DOS's own low
;--- memory layer writes exactly this shape.

MCB_FIRST       EQU 8                   ; the "owner" of a block DOS keeps

;=====================================================================
; mcb_walk - visit every block.  Entry: nothing.  Each iteration
;            leaves BX = the header's segment and AX = its length.
;            CF set when the chain is broken.
;
; It is a macro-shaped loop rather than a routine because every caller
; wants to do something different in the middle, and a callback in
; 16-bit assembly costs more than the four lines it saves.
;=====================================================================

;=====================================================================
; 48h - allocate BX paragraphs -> AX = the segment after the header.
;       On failure BX comes back with the largest block there is.
;=====================================================================
fn48:
        mov     bx,[bp+F_BX]
        call    mem_alloc
        jc      f48_bad
        mov     [bp+F_AX],ax
        jmp     i21_ok
f48_bad:
        mov     [bp+F_BX],bx            ; the biggest on offer
        mov     ax,ERR_NOMEM
        jmp     i21_fail

;--- mem_alloc - BX = paragraphs -> AX = segment, CF set if it will not
;--- fit and BX = the largest free run.
mem_alloc:
        push    cx
        push    dx
        push    si
        push    di
        push    es
        mov     cx,bx                   ; what was asked for
        xor     di,di                   ; the best so far, 0 = none
        xor     dx,dx                   ; the largest seen
        mov     bx,[h_mcbhead]
ma_walk:
        mov     es,bx
        mov     al,es:[RMCB_SIG]
        cmp     al,'M'
        je      ma_look
        cmp     al,'Z'
        jne     ma_broken
ma_look:
        cmp     word ptr es:[RMCB_OWN],0
        jne     ma_next
        call    mem_coalesce            ; free, so see if the next is too
        mov     ax,es:[RMCB_LEN]
        cmp     ax,dx
        jbe     ma_nobest
        mov     dx,ax
ma_nobest:
        cmp     ax,cx
        jb      ma_next
        ; Big enough.  First fit, which is what DOS's default strategy
        ; is and what nothing in Windows changes.
        mov     di,bx
        jmp     short ma_found
ma_next:
        cmp     byte ptr es:[RMCB_SIG],'Z'
        je      ma_end
        mov     ax,es:[RMCB_LEN]
        add     bx,ax
        inc     bx
        jmp     short ma_walk
ma_end:
        xor     di,di
ma_found:
        or      di,di
        jz      ma_none

        mov     es,di
        mov     ax,es:[RMCB_LEN]
        cmp     ax,cx
        je      ma_exact

        ; Split it: a new header cx paragraphs along, carrying what is
        ; left and the marker this block used to have.
        mov     bx,di
        add     bx,cx
        inc     bx                      ; the new header's own paragraph
        push    ax
        push    es
        mov     es,bx
        pop     bx                      ; the old header
        push    bx
        mov     bx,es
        pop     si
        push    es
        mov     es,si                   ; back to the old header
        mov     al,es:[RMCB_SIG]
        pop     es
        mov     es:[RMCB_SIG],al        ; the tail inherits M or Z
        mov     word ptr es:[RMCB_OWN],0
        pop     ax
        sub     ax,cx
        dec     ax
        mov     es:[RMCB_LEN],ax

        mov     es,di
        mov     byte ptr es:[RMCB_SIG],'M'
        mov     es:[RMCB_LEN],cx
ma_exact:
        mov     es,di
        mov     ax,[rd_psp]
        mov     es:[RMCB_OWN],ax
        mov     ax,di
        inc     ax
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        clc
        ret
ma_none:
        mov     bx,dx
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        stc
        ret
ma_broken:
        xor     bx,bx
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        stc
        ret

;--- mem_coalesce - ES = a free block's header.  Absorb the free blocks
;--- after it, so that a chain of small frees becomes one big one.
;
; WITHOUT THIS, FREEING BACKWARDS FRAGMENTS THE ARENA FOR GOOD.  DOS
; joins adjacent free blocks when it looks for space rather than when
; it frees, because a free does not know what is next to it and a
; search is walking past it anyway.
mem_coalesce:
        push    ax
        push    bx
        push    cx
        push    es
        push    ds
mc_more:
        cmp     byte ptr es:[RMCB_SIG],'Z'
        je      mc_done
        mov     bx,es
        mov     ax,es:[RMCB_LEN]
        add     bx,ax
        inc     bx
        push    es
        mov     es,bx
        cmp     word ptr es:[RMCB_OWN],0
        jne     mc_stop
        mov     cx,es:[RMCB_LEN]
        mov     bl,es:[RMCB_SIG]
        pop     es
        add     ax,cx
        inc     ax                      ; the absorbed header counts too
        mov     es:[RMCB_LEN],ax
        mov     es:[RMCB_SIG],bl        ; and its marker comes with it
        jmp     short mc_more
mc_stop:
        pop     es
mc_done:
        pop     ds
        pop     es
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; 49h - free the block whose data segment is ES
;=====================================================================
fn49:
        mov     es,[bp+F_ES]
        mov     ax,es
        dec     ax
        mov     es,ax
        mov     al,es:[RMCB_SIG]
        cmp     al,'M'
        je      f49_ok
        cmp     al,'Z'
        je      f49_ok
        mov     ax,ERR_BADMCB
        jmp     i21_fail
f49_ok:
        mov     word ptr es:[RMCB_OWN],0
        jmp     i21_ok

;=====================================================================
; 4Ah - resize the block at ES to BX paragraphs
;=====================================================================
fn4A:
        mov     es,[bp+F_ES]
        mov     bx,[bp+F_BX]
        mov     ax,es
        dec     ax
        mov     es,ax
        mov     al,es:[RMCB_SIG]
        cmp     al,'M'
        je      f4A_hdr
        cmp     al,'Z'
        je      f4A_hdr
        mov     ax,ERR_BADMCB
        jmp     i21_fail
f4A_hdr:
        call    mem_coalesce            ; grow into what is free after it
        mov     ax,es:[RMCB_LEN]
        cmp     bx,ax
        je      f4A_done
        ja      f4A_toobig

        ; Shrinking: a new free header where the block now ends
        push    ax
        mov     cx,es
        add     cx,bx
        inc     cx
        mov     dl,es:[RMCB_SIG]
        mov     byte ptr es:[RMCB_SIG],'M'
        mov     es:[RMCB_LEN],bx
        pop     ax
        sub     ax,bx
        dec     ax
        push    es
        mov     es,cx
        mov     es:[RMCB_SIG],dl
        mov     word ptr es:[RMCB_OWN],0
        mov     es:[RMCB_LEN],ax
        pop     es
f4A_done:
        jmp     i21_ok
f4A_toobig:
        mov     [bp+F_BX],ax            ; the most it can be
        mov     ax,ERR_NOMEM
        jmp     i21_fail

;=====================================================================
; 58h - the allocation strategy.  Answered, and then ignored.
;
; PM-DOS acts on this because it has upper memory to put things in.
; RMDOS has one chain and no UMBs - the pages PM-DOS mapped up there
; were PAGES, and paging stopped when we parked - so first fit is the
; only strategy there is.  Remembering what was asked for and reporting
; it back is not a pretence: a program that sets a strategy and reads
; it back to check gets a consistent answer, and one that sets it in
; order to place a block somewhere specific has nowhere to place it
; either way.
;=====================================================================
fn58:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jz      f58_getstrat
        cmp     al,1
        je      f58_setstrat
        cmp     al,2
        je      f58_getlink
        cmp     al,3
        je      f58_setlink
        jmp     i21_unsup
f58_getstrat:
        movzx   ax,byte ptr [rd_strat]
        mov     [bp+F_AX],ax
        jmp     i21_ok
f58_setstrat:
        mov     al,byte ptr [bp+F_BX]
        mov     [rd_strat],al
        jmp     i21_ok
f58_getlink:
        mov     word ptr [bp+F_AX],0    ; no upper memory to link
        jmp     i21_ok
f58_setlink:
        jmp     i21_ok

;=====================================================================
; 26h - build a PSP at DX, copied from ours
;=====================================================================
fn26:
        push    es
        push    ds
        mov     ax,[bp+F_DX]
        mov     es,ax
        mov     ds,[rd_psp]
        xor     si,si
        xor     di,di
        mov     cx,80h
        cld
        rep     movsb
        pop     ds
        mov     es,ax
        mov     word ptr es:[0],20CDh   ; INT 20h, as DOS has always left it
        pop     es
        jmp     i21_ok

;=====================================================================
; 50h - set the current PSP; 51h and 62h read it back
;=====================================================================
fn50:
        mov     ax,[bp+F_BX]
        mov     [rd_psp],ax
        jmp     i21_ok

fn51:
        mov     ax,[rd_psp]
        mov     [bp+F_BX],ax
        jmp     i21_ok

;=====================================================================
; 4Ch - terminate with the code in AL
; 31h - terminate and stay resident, DX paragraphs
;
; AND THIS IS WHERE THE EXCURSION ENDS.  There is no shell here to
; return to: RMDOS was loaded to run one thing, and when that thing is
; over the machine goes back to PM-DOS.  A program EXECed by another
; program is different - see fn4B, which puts a return address in
; rd_parent and gets control back at the label below.
;=====================================================================
fn4C:
        mov     al,byte ptr [bp+F_AX]
        movzx   ax,al
        mov     [rd_exitcode],ax
        mov     byte ptr [rd_exitway],0
        jmp     short term_common

fn31:
        mov     al,byte ptr [bp+F_AX]
        movzx   ax,al
        mov     ah,3                    ; DOS records the WAY it ended, and
        mov     [rd_exitcode],ax        ; 4Dh reports both halves
        mov     byte ptr [rd_exitway],3

        ; Keep DX paragraphs of the current PSP's block, then fall
        ; through: staying resident still ends this program.
        mov     bx,[bp+F_DX]
        cmp     bx,11h                  ; nothing smaller than a PSP
        jae     f31_size
        mov     bx,11h
f31_size:
        push    es
        mov     ax,[rd_psp]
        dec     ax
        mov     es,ax
        cmp     byte ptr es:[RMCB_SIG],'M'
        je      f31_cut
        cmp     byte ptr es:[RMCB_SIG],'Z'
        jne     f31_nocut
f31_cut:
        mov     ax,es:[RMCB_LEN]
        cmp     bx,ax
        jae     f31_nocut
        push    ax
        mov     cx,es
        add     cx,bx
        inc     cx
        mov     dl,es:[RMCB_SIG]
        mov     byte ptr es:[RMCB_SIG],'M'
        mov     es:[RMCB_LEN],bx
        pop     ax
        sub     ax,bx
        dec     ax
        push    es
        mov     es,cx
        mov     es:[RMCB_SIG],dl
        mov     word ptr es:[RMCB_OWN],0
        mov     es:[RMCB_LEN],ax
        pop     es
f31_nocut:
        pop     es
        jmp     short term_resident

term_common:
        ; Everything this program had open, closed; everything it
        ; allocated, freed.  A program that leaves a file open and
        ; exits has always had DOS clean up after it.
        call    proc_close
        call    proc_free
term_resident:
        call    rd_flush

        ; Is there a parent to go back to?
        cmp     word ptr [rd_parseg],0
        jne     term_toparent

        ; No.  The excursion is over.
        jmp     rd_goback

term_toparent:
        ; An EXECed program has ended.  Put back the caller's PSP, its
        ; stack, and the three vectors DOS saves across an EXEC, then
        ; return through the far address 4Bh left behind.
        cli
        mov     ax,[rd_parpsp]
        mov     [rd_psp],ax
        call    vec_restore
        mov     ax,[rd_parseg]
        mov     bx,[rd_paroff]
        mov     word ptr [rd_parret + 3],ax
        mov     word ptr [rd_parret + 1],bx
        mov     word ptr [rd_parseg],0
        mov     ax,[rd_parss]
        mov     bx,[rd_parsp]
        mov     ss,ax
        mov     sp,bx
        sti
rd_parret:
        db      0EAh
        dw      0
        dw      0

;--- proc_close - close every handle the current process holds -------
proc_close:
        pusha
        push    es
        mov     es,[rd_psp]
        xor     bx,bx
pc_loop:
        cmp     bx,JFT_COUNT
        jae     pc_done
        mov     al,es:[bx+18h]
        cmp     al,0FFh
        je      pc_next
        push    bx
        call    sft_release
        pop     bx
        mov     byte ptr es:[bx+18h],0FFh
pc_next:
        inc     bx
        jmp     short pc_loop
pc_done:
        pop     es
        popa
        ret

;--- proc_free - free every arena block this process owns ------------
proc_free:
        pusha
        push    es
        mov     dx,[rd_psp]
        mov     bx,[h_mcbhead]
pf_walk:
        mov     es,bx
        mov     al,es:[RMCB_SIG]
        cmp     al,'M'
        je      pf_look
        cmp     al,'Z'
        jne     pf_done
pf_look:
        cmp     es:[RMCB_OWN],dx
        jne     pf_next
        mov     word ptr es:[RMCB_OWN],0
pf_next:
        cmp     byte ptr es:[RMCB_SIG],'Z'
        je      pf_done
        mov     ax,es:[RMCB_LEN]
        add     bx,ax
        inc     bx
        jmp     short pf_walk
pf_done:
        pop     es
        popa
        ret

;=====================================================================
; 4Dh - the code the last program ended with.
;
; AL = the code, AH = HOW it ended: 0 normal, 1 Ctrl-C, 2 critical
; error, 3 resident.  And it answers ONCE - a second call reports
; nothing, because DOS clears it, and a program that asks twice is
; asking about a program that has not run.
;=====================================================================
fn4D:
        mov     ax,[rd_exitcode]
        mov     [bp+F_AX],ax
        mov     word ptr [rd_exitcode],0
        jmp     i21_ok

;=====================================================================
; 4Bh - EXEC.  DS:DX -> the name, ES:BX -> the parameter block.
;
;   AL=00h  load and run it
;   AL=01h  load it and hand back the frame, without running it
;   AL=03h  load it as an overlay: no PSP, no relocation of ours
;
; WHY WINDOWS NEEDS THIS AT ALL: every DOS box it opens is
; COMMAND.COM, started through here, and every DOS application in one
; is started by that.  A DOS underneath Windows with no EXEC is a DOS
; that can run Windows and nothing else.
;=====================================================================
fn4B:
        mov     al,byte ptr [bp+F_AX]
        cmp     al,0
        je      f4B_go
        cmp     al,1
        je      f4B_go
        cmp     al,3
        je      f4B_go
        jmp     i21_unsup
f4B_go:
        mov     [rd_execmode],al

        ; Open it first: a name that is not there costs nothing to
        ; discover and everything below assumes a handle.
        mov     ax,[bp+F_DS]
        mov     es,ax
        mov     si,[bp+F_DX]
        mov     ax,3D00h
        call    sub_open                ; -> BX = handle, CF on failure
        jc      f4B_nofile
        mov     [rd_exechnd],bx

        ; The header decides what kind of program it is.
        mov     cx,1Ch
        mov     dx,OFFSET rd_exehdr
        push    cs
        pop     es
        call    sub_read
        jc      f4B_bad

        cmp     word ptr [rd_exehdr],5A4Dh      ; 'MZ'
        je      f4B_exe
        cmp     word ptr [rd_exehdr],4D5Ah      ; and the other spelling,
        je      f4B_exe                         ; which DOS also accepted
        jmp     f4B_com

;--- an .EXE -----------------------------------------------------------
f4B_exe:
        ; How big is the image, in paragraphs?  The header says how
        ; many 512-byte pages and how many bytes are used in the last.
        movzx   eax,word ptr [rd_exehdr+4]      ; pages
        shl     eax,9
        movzx   ecx,word ptr [rd_exehdr+2]      ; bytes in the last page
        or      ecx,ecx
        jz      f4B_pages
        sub     eax,512
        add     eax,ecx
f4B_pages:
        movzx   ecx,word ptr [rd_exehdr+8]      ; the header's own size
        shl     ecx,4
        sub     eax,ecx
        mov     dword ptr [rd_execsize],eax     ; the load image, in bytes
        mov     dword ptr [rd_exechdrsz],ecx

        add     eax,15
        shr     eax,4
        mov     dword ptr [rd_execparas],eax

        ; The PSP, the image, and whatever the header says it wants
        ; beyond that.
        movzx   ebx,word ptr [rd_exehdr+0Ah]    ; minimum extra paragraphs
        add     eax,ebx
        add     eax,10h                         ; the PSP itself
        cmp     eax,0FFFFh
        ja      f4B_nomem
        mov     bx,ax
        call    mem_alloc
        jnc     f4B_got

        ; Not enough for the minimum.  Try for everything there is,
        ; which is what DOS did and what most .EXEs actually want.
        call    mem_alloc
        jc      f4B_nomem
f4B_got:
        mov     [rd_execpsp],ax
        mov     bx,ax
        add     bx,10h
        mov     [rd_execimg],bx

        ; Read the image in, in chunks that fit under 64K
        mov     eax,dword ptr [rd_exechdrsz]
        mov     bx,[rd_exechnd]
        call    sub_seek
        jc      f4B_bad
        mov     ax,[rd_execimg]
        mov     dword ptr [rd_execleft],0
        mov     eax,dword ptr [rd_execsize]
        mov     dword ptr [rd_execleft],eax
        mov     dx,0
        mov     ax,[rd_execimg]
        mov     [rd_execseg],ax
f4B_rdloop:
        cmp     dword ptr [rd_execleft],0
        je      f4B_loaded
        mov     ecx,dword ptr [rd_execleft]
        cmp     ecx,8000h
        jbe     f4B_chunk
        mov     ecx,8000h
f4B_chunk:
        mov     es,[rd_execseg]
        xor     dx,dx
        mov     bx,[rd_exechnd]
        push    ecx
        call    sub_read
        pop     ecx
        jc      f4B_bad
        sub     dword ptr [rd_execleft],ecx
        shr     ecx,4
        mov     ax,[rd_execseg]
        add     ax,cx
        mov     [rd_execseg],ax
        jmp     short f4B_rdloop
f4B_loaded:

        ; Relocations.  Each is a far pointer into the image whose
        ; segment word gets the load address added to it - which is the
        ; whole of what makes an .EXE different from a .COM.
        movzx   ecx,word ptr [rd_exehdr+6]      ; how many
        or      ecx,ecx
        jz      f4B_noreloc
        movzx   eax,word ptr [rd_exehdr+18h]    ; where the table is
        mov     bx,[rd_exechnd]
        call    sub_seek
        jc      f4B_bad
f4B_relloop:
        or      ecx,ecx
        jz      f4B_noreloc
        push    ecx
        mov     bx,[rd_exechnd]
        mov     cx,4
        mov     dx,OFFSET rd_relbuf
        push    cs
        pop     es
        call    sub_read
        pop     ecx
        jc      f4B_bad
        push    ecx
        mov     ax,word ptr [rd_relbuf+2]        ; the segment part
        add     ax,[rd_execimg]
        mov     es,ax
        mov     bx,word ptr [rd_relbuf]
        mov     ax,es:[bx]
        add     ax,[rd_execimg]
        mov     es:[bx],ax
        pop     ecx
        dec     ecx
        jmp     short f4B_relloop
f4B_noreloc:

        mov     ax,word ptr [rd_exehdr+0Eh]      ; the initial SS, relocated
        add     ax,[rd_execimg]
        mov     [rd_execss],ax
        mov     ax,word ptr [rd_exehdr+10h]
        mov     [rd_execsp],ax
        mov     ax,word ptr [rd_exehdr+16h]      ; and CS:IP
        add     ax,[rd_execimg]
        mov     [rd_execcs],ax
        mov     ax,word ptr [rd_exehdr+14h]
        mov     [rd_execip],ax
        jmp     f4B_start

;--- a .COM, which is everything that is not an .EXE -----------------
f4B_com:
        mov     bx,0FFFFh
        call    mem_alloc               ; ask for the impossible to learn
        mov     ax,bx                   ; how much there is
        cmp     ax,11h
        jb      f4B_nomem
        mov     bx,ax
        call    mem_alloc
        jc      f4B_nomem
        mov     [rd_execpsp],ax
        mov     bx,ax
        add     bx,10h
        mov     [rd_execimg],bx

        xor     eax,eax
        mov     bx,[rd_exechnd]
        call    sub_seek
        jc      f4B_bad
        mov     es,[rd_execimg]
        mov     dx,100h
        mov     cx,0FF00h
        call    sub_read
        jc      f4B_bad

        mov     ax,[rd_execpsp]
        mov     [rd_execcs],ax
        mov     [rd_execss],ax
        mov     word ptr [rd_execip],100h
        mov     word ptr [rd_execsp],0FFFEh

f4B_start:
        mov     bx,[rd_exechnd]
        call    sub_close

        cmp     byte ptr [rd_execmode],3
        je      f4B_overlay

        call    exec_psp                ; build it and copy the tail over
        jc      f4B_nomem

        cmp     byte ptr [rd_execmode],1
        jne     f4B_run

        ; AL=1: hand the frame back and run nothing.  The caller wanted
        ; to load a program and start it itself, which is what a
        ; debugger does.
        mov     es,[bp+F_ES]
        mov     bx,[bp+F_BX]
        mov     ax,[rd_execss]
        mov     es:[bx+0Eh],ax
        mov     ax,[rd_execsp]
        mov     es:[bx+0Ch],ax
        mov     ax,[rd_execcs]
        mov     es:[bx+12h],ax
        mov     ax,[rd_execip]
        mov     es:[bx+10h],ax
        jmp     i21_ok

f4B_overlay:
        ; No PSP, no new process: the image was read straight into the
        ; segment the caller named, and that is the whole call.
        jmp     i21_ok

;---------------------------------------------------------------------
; And run it.  THE PARENT'S WHOLE WORLD IS SAVED HERE, because the
; child will change all of it and DOS promises to put it back: the
; PSP, the stack, and vectors 22h, 23h and 24h.
;---------------------------------------------------------------------
f4B_run:
        cli
        mov     ax,[rd_psp]
        mov     [rd_parpsp],ax
        mov     ax,[bp+F_OWNSEG]        ; where to come back to: the frame
        mov     [rd_parss],ax           ; this call was made through
        mov     ax,[bp+F_OWNOFF]
        mov     [rd_parsp],ax
        mov     word ptr [rd_parseg],cs
        mov     word ptr [rd_paroff],OFFSET f4B_back
        call    vec_save

        mov     ax,[rd_execpsp]
        mov     [rd_psp],ax
        mov     [rd_dtaseg],ax
        mov     word ptr [rd_dtaoff],80h

        mov     ax,[rd_execss]
        mov     bx,[rd_execsp]
        mov     ss,ax
        mov     sp,bx
        mov     ax,[rd_execip]
        mov     word ptr cs:[f4B_jmp + 1],ax
        mov     ax,[rd_execcs]
        mov     word ptr cs:[f4B_jmp + 3],ax
        mov     ax,[rd_execpsp]
        mov     ds,ax
        mov     es,ax
        xor     ax,ax
        sti
f4B_jmp:
        db      0EAh
        dw      0
        dw      0

; The child ended and term_toparent came back here.  The frame is
; where it was; the answer is success.
f4B_back:
        mov     ax,cs
        mov     ds,ax
        mov     es,ax
        mov     ss,ax
        mov     sp,OFFSET rd_stktop
        mov     word ptr [h_why],RMW_EXIT
        ; The 4Bh call that started the child is over.  There is no
        ; frame to answer through any more - the stack it was on has
        ; been unwound - so the excursion ends here too.
        jmp     rd_goback

f4B_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f4B_nomem:
        mov     bx,[rd_exechnd]
        call    sub_close
        mov     ax,ERR_NOMEM
        jmp     i21_fail
f4B_bad:
        mov     bx,[rd_exechnd]
        call    sub_close
        mov     ax,ERR_EXEFMT
        jmp     i21_fail

;---------------------------------------------------------------------
; exec_psp - build the child's PSP from the parameter block at ES:BX
;            as the caller gave it.  CF on failure.
;---------------------------------------------------------------------
exec_psp:
        pusha
        push    es
        push    ds

        mov     es,[rd_execpsp]
        xor     di,di
        mov     cx,80h
        xor     al,al
        cld
        rep     stosb

        mov     word ptr es:[0],20CDh           ; INT 20h
        mov     ax,[rd_execpsp]
        mov     bx,ax
        dec     bx
        push    es
        mov     es,bx
        mov     cx,es:[RMCB_LEN]
        pop     es
        add     ax,cx
        inc     ax
        mov     es:[2],ax                       ; the paragraph after it
        mov     byte ptr es:[5],9Ah             ; a far call to the
        mov     word ptr es:[6],0FEF0h          ; dispatcher, as DOS left it
        mov     word ptr es:[8],0F01Dh

        mov     ax,[rd_psp]                     ; the parent
        mov     es:[16h],ax

        ; Every handle is closed to start with, then the parent's first
        ; five are inherited - which is what makes redirection work
        ; across an EXEC and is the whole reason a JFT is a table of
        ; indexes rather than of open files.
        mov     cx,JFT_COUNT
        mov     di,18h
        mov     al,0FFh
        rep     stosb
        mov     word ptr es:[32h],JFT_COUNT
        mov     word ptr es:[34h],18h
        mov     ax,[rd_execpsp]
        mov     word ptr es:[36h],ax

        push    es
        mov     ds,[rd_psp]
        pop     es
        mov     si,18h
        mov     di,18h
        mov     cx,5
ep_inherit:
        mov     al,[si]
        mov     es:[di],al
        cmp     al,0FFh
        je      ep_nextj
        push    cx
        push    si
        push    di
        call    sft_addref
        pop     di
        pop     si
        pop     cx
ep_nextj:
        inc     si
        inc     di
        loop    ep_inherit

        push    cs
        pop     ds

        ; The environment and the command tail, out of the parameter
        ; block the caller passed.
        mov     es,[bp+F_ES]
        mov     bx,[bp+F_BX]
        mov     ax,es:[bx]                      ; the environment segment
        or      ax,ax
        jnz     ep_haveenv
        mov     ax,[rd_env]                     ; 0 means "a copy of mine"
ep_haveenv:
        push    ax
        mov     ax,es:[bx+4]
        mov     dx,es:[bx+2]
        mov     word ptr [ep_tail],dx
        mov     word ptr [ep_tail+2],ax
        pop     ax

        push    es
        mov     es,[rd_execpsp]
        mov     es:[2Ch],ax                     ; the environment
        pop     es

        ; The command tail, all 128 bytes of it, to PSP:80h
        push    ds
        lds     si,dword ptr [ep_tail]
        mov     ax,[rd_execpsp]
        mov     es,ax
        mov     di,80h
        mov     cx,80h
        cld
        rep     movsb
        pop     ds

        pop     ds
        pop     es
        popa
        clc
        ret

;--- vec_save / vec_restore - 22h, 23h and 24h across an EXEC --------
vec_save:
        pusha
        push    es
        xor     ax,ax
        mov     es,ax
        mov     eax,dword ptr es:[22h*4]
        mov     dword ptr [rd_v22],eax
        mov     eax,dword ptr es:[23h*4]
        mov     dword ptr [rd_v23],eax
        mov     eax,dword ptr es:[24h*4]
        mov     dword ptr [rd_v24],eax
        pop     es
        popa
        ret

vec_restore:
        pusha
        push    es
        xor     ax,ax
        mov     es,ax
        mov     eax,dword ptr [rd_v22]
        mov     dword ptr es:[22h*4],eax
        mov     eax,dword ptr [rd_v23]
        mov     dword ptr es:[23h*4],eax
        mov     eax,dword ptr [rd_v24]
        mov     dword ptr es:[24h*4],eax
        pop     es
        popa
        ret

;=====================================================================
; Data
;=====================================================================
rd_strat    db  0
rd_env      dw  0

rd_parpsp   dw  0
rd_parseg   dw  0
rd_paroff   dw  0
rd_parss    dw  0
rd_parsp    dw  0

rd_v22      dd  0
rd_v23      dd  0
rd_v24      dd  0

rd_execmode db  0
rd_exechnd  dw  0
rd_execpsp  dw  0
rd_execimg  dw  0
rd_execseg  dw  0
rd_execcs   dw  0
rd_execip   dw  0
rd_execss   dw  0
rd_execsp   dw  0
rd_execsize dd  0
rd_exechdrsz dd 0
rd_execparas dd 0
rd_execleft dd  0
rd_exehdr   db  32 dup(0)
rd_relbuf   db  4 dup(0)
ep_tail     dd  0
