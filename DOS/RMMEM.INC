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
; 55h - build a CHILD PSP at DX, with SI as the top of its memory
;
; 55h IS UNDOCUMENTED AND IT IS NOT OPTIONAL.  Windows creates a PSP
; for every task it runs and it creates them with 55h, not with 4Bh:
; Program Manager lives in one, and so does every program it starts.
; RMDOS answered "unsupported", so the PSP Windows got back was never
; initialised - PSP:32h, the number of handles the process is allowed,
; came back as whatever happened to be in that memory, which was zero
; - and every file the shell tried to open was refused with "too many
; open files".  Twenty-four refusals of PROGMAN.INI in a row, and a
; Program Manager with no groups in it.  That is the whole of v0.54's
; "what is still wrong".
;
; 26h is the same routine without the process: the copy, the INT 20h,
; the handle table, and no parent, no inheritance and no change of
; current process.  Both follow $Dup_PDB and
; $CREATE_PROCESS_DATA_BLOCK in the MS-DOS 6 source, DOS/misc.asm.
;
; AND THE COPY IS 256 BYTES.  It was 128, which stops four bytes short
; of PSP:34h - the pointer that says where the handle table is.  DOS
; copies 80h WORDS; this copied 80h BYTES.
;=====================================================================
fn55:
        mov     byte ptr [rd_pspchild],1
        jmp     short f26_common
fn26:
        mov     byte ptr [rd_pspchild],0
f26_common:
        push    es
        push    ds
        push    si
        push    di
        mov     ax,[bp+F_DX]
        mov     [rd_pspnew],ax

;--- the whole of it
        mov     es,ax
        mov     ds,[rd_psp]
        xor     si,si
        xor     di,di
        mov     cx,100h
        cld
        rep     movsb
        push    cs
        pop     ds

;--- the handles the parent has, from wherever the parent keeps them
        mov     es,[rd_psp]
        call    jft_ptr                 ; -> ES:DI the parent's, CX of them
        mov     si,di
        mov     ax,es
        mov     ds,ax
        mov     es,word ptr cs:[rd_pspnew]
        mov     di,18h
        cmp     cx,JFT_COUNT
        jbe     f26_fits
        mov     cx,JFT_COUNT            ; only the first twenty travel
f26_fits:
        cld
        rep     movsb
        push    cs
        pop     ds

;--- ...and the table the child will use is its OWN, which is the
;--- whole point: a child that inherits its parent's POINTER shares
;--- its parent's table and closes its parent's files.
        mov     es,[rd_pspnew]
        mov     word ptr es:[0],20CDh   ; INT 20h, as DOS has always left it
        mov     word ptr es:[32h],JFT_COUNT
        mov     word ptr es:[34h],18h
        mov     ax,[rd_pspnew]
        mov     word ptr es:[36h],ax

        cmp     byte ptr [rd_pspchild],0
        je      f26_done

;--- a CHILD: the top of its memory comes from SI, it names its
;--- parent, it holds a reference to every handle it inherited, and it
;--- becomes the current process.
        mov     ax,[bp+F_SI]
        mov     word ptr es:[2],ax
        mov     ax,[rd_psp]
        mov     word ptr es:[16h],ax
        xor     bx,bx
f55_ref:
        cmp     bx,JFT_COUNT
        jae     f55_reffed
        mov     al,es:[bx+18h]
        cmp     al,0FFh
        je      f55_refnext
        push    bx
        push    es
        call    sft_addref
        pop     es
        pop     bx
f55_refnext:
        inc     bx
        jmp     short f55_ref
f55_reffed:
        mov     ax,[rd_pspnew]
        mov     [rd_psp],ax
f26_done:
        pop     di
        pop     si
        pop     ds
        pop     es
        jmp     i21_ok

rd_pspnew   dw  0                       ; 26h/55h: the PSP being built
rd_pspchild db  0                       ; ...and whether it is a process

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
        call    jft_ptr                 ; -> ES:DI the table, CX its size
        xor     bx,bx
pc_loop:
        cmp     bx,cx
        jae     pc_done
        mov     al,es:[bx+di]
        cmp     al,0FFh
        je      pc_next
        push    bx
        push    cx
        push    di
        call    sft_release
        pop     di
        pop     cx
        pop     bx
        mov     byte ptr es:[bx+di],0FFh
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

IFDEF RMDOS_EDBG
;=====================================================================
; The EXEC trace.  Straight to the BIOS teletype, because on THIS side
; of the handover there is a screen again.
;
; Windows takes the display into 640x480 before it EXECs anything, and
; the only instrument that survives that is a bar painted into the
; framebuffer and read off a screenshot - one boot per reading, and
; four releases of evidence that reading it costs more than building
; it.  PARKEXEC.COM parks from a TEXT MODE prompt and then makes the
; same call Windows makes, so the same code can simply PRINT.  That is
; the whole reason PARKEXEC exists, and this is what it buys.
;
; rd_say is used rather than the console: the console is a thing this
; DOS provides to programs, and a program is exactly what is being
; loaded here.
;=====================================================================
ed_say:                                 ; SI -> ASCIZ in this segment
        push    ds
        push    cs
        pop     ds
        call    rd_say
        pop     ds
        ret

ed_hex:                                 ; AX in hex, four digits
        push    ax
        push    bx
        push    cx
        mov     cx,4
eh_one:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     eh_dig
        add     al,7
eh_dig:
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        pop     ax
        loop    eh_one
        pop     cx
        pop     bx
        pop     ax
        ret

;--- SI -> a key, AX = its value, and a space after it.  rd_say keeps
;--- both registers, so a run of these needs no reloading between them.
ed_kv:
        call    ed_say
        call    ed_hex
        push    si
        mov     si,OFFSET ed_msp
        call    ed_say
        pop     si
        ret

ed_nl:
        push    si
        mov     si,OFFSET ed_mnl
        call    ed_say
        pop     si
        ret

;--- ed_peek AND ed_v21 ARE KEPT WITH NO CALLERS, deliberately.  Each
;--- of them found one of this release's two hardest bugs and was then
;--- taken back out, because a probe left in every path is noise and a
;--- probe deleted is a probe that has to be rewritten from memory next
;--- time.  Drop a call to one wherever the next question is.
;
;--- AL = a label, then two words out of the loaded image: the first,
;--- and the one at the entry point.  "The loader ran to completion" is
;--- not "the program is in memory".
ed_peek:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        mov     es,[rd_execimg]
        mov     si,OFFSET ed_kim0
        xor     bx,bx
        mov     ax,es:[bx]
        call    ed_kv
        mov     si,OFFSET ed_kim1
        mov     bx,0390h
        mov     ax,es:[bx]
        call    ed_kv
        pop     es
        pop     ds
        popa
        ret

;--- AL = a label character, then the INT 21h vector out of the table.
;--- Sprinkled through fn4B because the vector is RIGHT when the
;--- parent calls and WRONG by the time the child does, and the only
;--- useful question left is which instruction between the two did it.
ed_v21:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        xor     ax,ax
        mov     es,ax
        mov     si,OFFSET ed_kv21
        mov     ax,es:[21h*4 + 2]
        call    ed_kv
        mov     si,OFFSET ed_kv21b
        mov     ax,es:[21h*4]
        call    ed_kv
        pop     es
        pop     ds
        popa
        ret

;--- what was asked for, including the NAME, which is the one thing a
;--- bar cannot say and the first thing worth knowing when a loader is
;--- entered more than once.
ed_entry:
        pusha
        push    ds
        mov     si,OFFSET ed_m4b
        call    ed_say
        movzx   ax,byte ptr [rd_execmode]
        call    ed_hex
        mov     si,OFFSET ed_msp
        call    ed_say
        mov     ds,[bp+F_DS]
        mov     si,[bp+F_DX]
        mov     cx,16
ee_ch:
        lodsb
        or      al,al
        jz      ee_end
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        loop    ee_ch
ee_end:
        push    cs
        pop     ds
        call    ed_nl
        pop     ds
        popa
        ret

;--- where it is about to jump, and the first four bytes that are
;--- there.  A loader that has finished and a loader that has finished
;--- CORRECTLY are different claims: v0.50 read 4D 5A at this address,
;--- which is 'MZ', and meant the jump was into the header it had just
;--- parsed.
ed_target:
        pusha
        push    es
        mov     si,OFFSET ed_kcs
        mov     ax,[rd_execcs]
        call    ed_kv
        mov     si,OFFSET ed_kip
        mov     ax,[rd_execip]
        call    ed_kv
        mov     si,OFFSET ed_kss
        mov     ax,[rd_execss]
        call    ed_kv
        mov     si,OFFSET ed_ksp
        mov     ax,[rd_execsp]
        call    ed_kv
        mov     es,[rd_execcs]
        mov     bx,[rd_execip]
        mov     si,OFFSET ed_kent
        mov     ax,es:[bx]
        call    ed_kv
        mov     si,OFFSET ed_kent2
        mov     ax,es:[bx+2]
        call    ed_kv
        ; ...and the vector the child's first DOS call will follow,
        ; read out of the table rather than out of what we believe.
        xor     ax,ax
        mov     es,ax
        mov     si,OFFSET ed_kv21
        mov     ax,es:[21h*4 + 2]
        call    ed_kv
        mov     si,OFFSET ed_kv21b
        mov     ax,es:[21h*4]
        call    ed_kv
        call    ed_nl
        pop     es
        popa
        ret

;--- every INT 21h once EXEC has jumped, which is the question a bar
;--- in a framebuffer could never answer: did the CHILD ever run?  A
;--- call arriving with the child's CS says yes and moves the fault
;--- into RMDOS; no call at all says the jump went nowhere.
ed_i21:
        cmp     byte ptr [ed_post],0
        je      ei_no
        cmp     byte ptr [ed_left],0
        je      ei_no
        dec     byte ptr [ed_left]
        pusha
        mov     si,OFFSET ed_kin
        movzx   ax,byte ptr [bp+F_AX+1]
        call    ed_kv
        mov     si,OFFSET ed_kcs
        mov     ax,[bp+F_CS]
        call    ed_kv
        mov     si,OFFSET ed_kip
        mov     ax,[bp+F_IP]
        call    ed_kv
        call    ed_nl
        popa
ei_no:
        ret

; ed_post and ed_left are declared in RMDOS.ASM, ahead of i21 -
; see the note there.  They are read from BOTH files and one of the
; two reads has to be a forward reference unless they live first.
ed_kin      db  'IN=',0
ed_m4b      db  '4B:',0
ed_msp      db  ' ',0
ed_mnl      db  13,10,0
ed_kopen    db  'OPEN=',0
ed_khdr     db  'HDR=',0
ed_mexe     db  'EXE ',0
ed_mcom     db  'COM ',0
ed_kpsp     db  'PSP=',0
ed_kimg     db  'IMG=',0
ed_kmax     db  'MAX=',0
ed_kcs      db  'CS=',0
ed_kip      db  'IP=',0
ed_kss      db  'SS=',0
ed_ksp      db  'SP=',0
ed_kent     db  'ENT=',0
ed_kent2    db  '/',0
ed_kim0     db  'IM0=',0
ed_kim1     db  'IM390=',0
ed_ksz     db  'SZ=',0
ed_kpos    db  'POS=',0
ed_kgot    db  'GOT=',0
ed_kv21     db  'V21=',0
ed_kv21b    db  ':',0
ed_kjmp     db  'JMP=',0
ed_kjmp2    db  ':',0
ed_mgo      db  'GO',13,10,0
ed_mback    db  'BACK',13,10,0
ed_mnomem   db  'NOMEM',13,10,0
ed_mbad     db  'BAD',13,10,0
ed_mnof     db  'NOFILE',13,10,0
ENDIF

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
IFDEF RMDOS_EDBG
        call    ed_entry
ENDIF
IFDEF RMDOS_MARK
        mov     al,1
        call    rd_vstage
        mov     al,[bp+F_AX]
        mov     [rd_execmode],al
ENDIF

        ; Open it first: a name that is not there costs nothing to
        ; discover and everything below assumes a handle.
        mov     ax,[bp+F_DS]
        mov     es,ax
        mov     si,[bp+F_DX]
        mov     ax,3D00h
        call    sub_open                ; -> BX = handle, CF on failure
        jc      f4B_nofile
        mov     [rd_exechnd],bx
IFDEF RMDOS_EDBG
        push    si
        mov     si,OFFSET ed_kopen
        mov     ax,bx
        call    ed_kv
        pop     si
ENDIF

        ; The header decides what kind of program it is.
        mov     ecx,1Ch                 ; ECX, NOT CX: file_io reads all
        mov     dx,OFFSET rd_exehdr     ; thirty-two bits of it
        push    cs
        pop     es
        call    sub_read
        jc      f4B_bad

IFDEF RMDOS_EDBG
        push    si
        mov     si,OFFSET ed_khdr
        mov     ax,word ptr [rd_exehdr]
        call    ed_kv
        pop     si
ENDIF
        cmp     word ptr [rd_exehdr],5A4Dh      ; 'MZ'
        je      f4B_exe
        cmp     word ptr [rd_exehdr],4D5Ah      ; and the other spelling,
        je      f4B_exe                         ; which DOS also accepted
        jmp     f4B_com

;--- an .EXE -----------------------------------------------------------
f4B_exe:
IFDEF RMDOS_MARK
        push    ax
        mov     al,02h                  ; the .EXE branch
        call    rd_vstage
        pop     ax
ENDIF
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

        ; The PSP, the image, and what the header asks for beyond it.
        ;
        ; THERE ARE TWO NUMBERS IN AN .EXE HEADER AND THIS USED TO READ
        ; ONLY ONE.  MINALLOC is what the program cannot run without;
        ; MAXALLOC is what it would like, and a linker writes FFFF
        ; there to mean "all of it", which is what almost every .EXE
        ; says.  Asking for MINALLOC and stopping gave HELLOX.EXE 1.3K
        ; on a machine with half a megabyte free, and gave any program
        ; that sizes its heap from the block a heap of nothing.
        ;
        ; So: ask for MAXALLOC, and if that will not fit take the
        ; largest run there is - which mem_alloc has already measured
        ; and left in BX - provided it covers MINALLOC.  Only then is
        ; it out of memory.
        mov     ecx,eax
        add     ecx,10h                         ; the PSP itself
        mov     eax,ecx

        ; MAXALLOC ZERO MEANS LOAD HIGH, and it is not a corner case:
        ; KRNL386.EXE says 0/0 and so does every .EXE built to sit at
        ; the top of memory with its free space BELOW it.  DOS gives
        ; such a program the largest block there is, puts the PSP at
        ; the bottom of it and THE IMAGE AT THE TOP (msproc.asm,
        ; Exec_Load_High) - so the program is handed a run of free
        ; paragraphs between its PSP and itself.  Loading it low
        ; instead leaves that run empty, and a program that sizes
        ; anything from it gets nothing.  Asking for FFFFh here takes
        ; the "largest run" path below, which is the block DOS would
        ; have chosen; f4B_got does the placing.
        movzx   ebx,word ptr [rd_exehdr+0Ch]    ; MAXALLOC - what it wants
        mov     byte ptr [rd_exechigh],0
        or      bx,bx
        jnz     f4B_maxed
        inc     byte ptr [rd_exechigh]
        mov     ebx,0FFFFh
f4B_maxed:
        add     eax,ebx
        cmp     eax,0FFFFh
        jbe     f4B_want
        mov     eax,0FFFFh                      ; a segment is all there is
f4B_want:
        movzx   ebx,word ptr [rd_exehdr+0Ah]    ; MINALLOC - what it needs
        add     ecx,ebx
        cmp     ecx,0FFFFh
        ja      f4B_nomem
        mov     bx,ax
        call    mem_alloc
        jnc     f4B_got
        cmp     bx,cx                           ; BX = the largest run
        jb      f4B_nomem
        call    mem_alloc
        jc      f4B_nomem
f4B_got:
        mov     [rd_execpsp],ax
        mov     bx,ax
        add     bx,10h
        mov     [rd_execimg],bx
        cmp     byte ptr [rd_exechigh],0
        je      f4B_placed
        push    es                              ; the top of the block, less
        mov     bx,ax                           ; the image, is where a
        dec     bx                              ; load-high image begins
        mov     es,bx
        mov     cx,es:[RMCB_LEN]
        pop     es
        add     cx,ax
        sub     cx,word ptr [rd_execparas]
        mov     [rd_execimg],cx
f4B_placed:
IFDEF RMDOS_EDBG
        push    si
        mov     si,OFFSET ed_mexe
        call    ed_say
        mov     si,OFFSET ed_kpsp
        mov     ax,[rd_execpsp]
        call    ed_kv
        mov     si,OFFSET ed_kimg
        mov     ax,[rd_execimg]
        call    ed_kv
        call    ed_nl
        pop     si
ENDIF

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
        call    sub_read
        jc      f4B_bad
IFDEF RMDOS_EDBG
        push    ecx
        push    si
        mov     si,OFFSET ed_ksz
        mov     ax,word ptr [ed_sz]
        call    ed_kv
        mov     si,OFFSET ed_kpos
        mov     ax,word ptr [ed_pos]
        call    ed_kv
        mov     si,OFFSET ed_kgot
        mov     ax,cx
        call    ed_kv
        pop     si
        pop     ecx
ENDIF
        ; WHAT CAME BACK, not what was asked for.  This used to push
        ; the request across the call and subtract that - so a short
        ; read counted as a whole one and the loop finished early with
        ; nothing to say about it.  A read that moves nothing at all
        ; ends the load rather than going round again for ever.
        or      ecx,ecx
        jz      f4B_loaded
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
        mov     ecx,4                   ; ECX - see the header read above
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
IFDEF RMDOS_MARK
        push    ax
        mov     al,4
        call    rd_vstage
        pop     ax
ENDIF
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
IFDEF RMDOS_EDBG
        push    si
        mov     si,OFFSET ed_mcom
        call    ed_say
        mov     si,OFFSET ed_kpsp
        mov     ax,[rd_execpsp]
        call    ed_kv
        mov     si,OFFSET ed_kimg
        mov     ax,[rd_execimg]
        call    ed_kv
        call    ed_nl
        pop     si
ENDIF

        xor     eax,eax
        mov     bx,[rd_exechnd]
        call    sub_seek
        jc      f4B_bad
        ; AND THE SECOND HALF OF THE SAME ACCIDENT.  A .COM is loaded
        ; at PSP:0100, which is execimg:0000 - execimg IS the PSP plus
        ; ten paragraphs, so the two spellings are the same address.
        ; This said execimg:0100, which is 256 bytes too high, and it
        ; worked for five releases because sft_get was zeroing the
        ; offset before the read ever saw it.  Two bugs that cancelled,
        ; on the only branch anything ever took: fixing sft_get on its
        ; own would have broken every .COM in the machine.
        mov     es,[rd_execimg]
        xor     dx,dx
        mov     ecx,0FF00h              ; ECX - see the header read above
        call    sub_read
        jc      f4B_bad

        mov     ax,[rd_execpsp]
        mov     [rd_execcs],ax
        mov     [rd_execss],ax
        mov     word ptr [rd_execip],100h
        mov     word ptr [rd_execsp],0FFFEh

f4B_start:
IFDEF RMDOS_MARK
        push    ax
        mov     al,5
        call    rd_vstage
        pop     ax
ENDIF
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
IFDEF RMDOS_MARK
        push    ax
        mov     al,6
        call    rd_vstage
        pop     ax
ENDIF
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

        ; DOS IS NOT BUSY ANY MORE, AND HAS TO SAY SO.
        ;
        ; This 4Bh will never reach i21_out - the whole point of it is
        ; that control leaves through the far jump below and comes
        ; back, if it ever does, at f4B_back on a different stack.  So
        ; the two counters i21 raised on the way in are raised for
        ; ever unless they are put down here, and one of them is the
        ; InDOS flag: the byte every well-behaved program in the world
        ; watches to find out whether it may talk to DOS.
        ;
        ; A child started with InDOS stuck at 1 sees a DOS that is
        ; permanently mid-call.  Windows is exactly such a program -
        ; v0.44 built the swappable data area so that it could FIND
        ; this byte - and what it does about it is wait, quietly,
        ; making no DOS calls at all, which is indistinguishable from
        ; a crash from outside and is not one.
        ;
        ; The stack depth goes with it: the internal stack this call
        ; borrowed is about to be abandoned along with everything else.
        mov     byte ptr [rd_indos],0
        mov     byte ptr [rd_stkdep],0

IFDEF RMDOS_EDBG
        call    ed_target
        push    si
        mov     si,OFFSET ed_mgo
        call    ed_say
        pop     si
        mov     byte ptr [ed_post],1    ; from here on, log what arrives
        mov     byte ptr [ed_left],8    ; ...with a fresh allowance
ENDIF
        mov     ax,[rd_execss]
        mov     bx,[rd_execsp]
        mov     ss,ax
        mov     sp,bx
        ; AN INDIRECT JUMP, NOT A PATCHED ONE.  This used to build the
        ; far jump below by writing its operands into the instruction
        ; itself, eight instructions before executing it - and this is
        ; a DOS living in the HMA, being run by a recompiling emulator
        ; on a processor with a prefetch queue.  Code that modifies the
        ; instruction about to be fetched is the one case where all
        ; three of those get to disagree, and it costs nothing to not
        ; ask: the address goes in a variable and the jump reads it.
        mov     ax,[rd_execip]
        mov     word ptr [rd_execjmp],ax
        mov     ax,[rd_execcs]
        mov     word ptr [rd_execjmp + 2],ax
        mov     ax,[rd_execpsp]
        mov     ds,ax
        mov     es,ax
IFDEF RMDOS_MARK
        push    ax
        mov     al,7                    ; loaded, relocated, PSP built,
        call    rd_vstage               ; segments set: the next instruction
        pop     ax                      ; is the child's own
ENDIF
IFDEF RMDOS_EDBG
        ; THE LAST THING BEFORE THE JUMP, AND IT READS THE JUMP BACK.
        ; Everything above is a report about what was intended; this is
        ; the instruction itself, out of the code that is about to run
        ; it - and RMDOS is in the HMA, so the patch that put it there
        ; was a write above the megabyte and is worth confirming.
        push    ds
        push    cs
        pop     ds
        push    si
        mov     si,OFFSET ed_kjmp
        mov     ax,word ptr [rd_execjmp]
        call    ed_kv
        mov     si,OFFSET ed_kjmp2
        mov     ax,word ptr [rd_execjmp + 2]
        call    ed_kv
        call    ed_nl
        pop     si
        pop     ds
        mov     ax,[rd_execpsp]         ; and put back what the trace used
        mov     ds,ax
        mov     es,ax
ENDIF
        xor     ax,ax
        sti
        jmp     dword ptr cs:[rd_execjmp]

; The child ended and term_toparent came back here.  The frame is
; where it was; the answer is success.
f4B_back:
        mov     ax,cs
        mov     ds,ax
IFDEF RMDOS_EDBG
        push    si
        mov     si,OFFSET ed_mback
        call    ed_say
        pop     si
ENDIF
        mov     es,ax

        ; AND NOW ANSWER THE 4Bh, which is what a DOS does.
        ;
        ; This used to jmp rd_goback - end the whole excursion and give
        ; the machine back to PM-DOS - on the reasoning that the frame
        ; the 4Bh was made through had been unwound with the stack.  It
        ; had not.  term_toparent has just put SS:SP back to exactly
        ; where i21 left the caller's own copy of the frame, so the
        ; frame is under our feet and the call can be returned from the
        ; ordinary way: fix up the answer in it and IRET.
        ;
        ; The difference matters to the only caller there has ever
        ; been.  A parent whose child ends does not want the machine
        ; taken away from it - it wants its EXEC to return, so that it
        ; can ask 4Dh what the child exited with and then get on with
        ; whatever it EXECs next.  Windows EXECs a chain of them.
        mov     bp,sp
        mov     word ptr [bp+F_AX],0
        and     word ptr [bp+F_FL],NOT 1        ; success: carry clear
        mov     byte ptr [rd_indos],0           ; the counters this call
        mov     byte ptr [rd_stkdep],0          ; raised and never lowered
        pop     es
        pop     ds
        popa
        iret

f4B_nofile:
IFDEF RMDOS_EDBG
        mov     si,OFFSET ed_mnof
        call    ed_say
ENDIF
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f4B_nomem:
IFDEF RMDOS_EDBG
        mov     si,OFFSET ed_mnomem
        call    ed_say
ENDIF
        mov     bx,[rd_exechnd]
        call    sub_close
        mov     ax,ERR_NOMEM
        jmp     i21_fail
f4B_bad:
IFDEF RMDOS_EDBG
        mov     si,OFFSET ed_mbad
        call    ed_say
ENDIF
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

        ; THE BLOCK CHANGES HANDS HERE, and this is the line that makes
        ; a child's memory the CHILD's.  fn4B allocates it before there
        ; is a PSP to own it, so mem_alloc stamped it with the parent's
        ; - and a block owned by the parent is a block the child's own
        ; termination will not free.  One EXEC and the arena was empty
        ; for good; the second EXEC in the same program failed for want
        ; of memory the first one had already finished with.
        mov     cx,[rd_execpsp]
        mov     es:[RMCB_OWN],cx

        mov     cx,es:[RMCB_LEN]
        pop     es
        add     ax,cx                           ; the paragraph after it,
        mov     es:[2],ax                       ; and NOT one more: the data
IFDEF DBG_LPT
        mov     word ptr cs:[rdbg_ctop],ax      ; what the child is told it owns
        push    ax
        mov     ax,[rd_execpsp]
        mov     word ptr cs:[rdbg_cpsp],ax
        pop     ax
ENDIF
                                                ; runs from the header's own
                                                ; paragraph plus one, so the
                                                ; end is base + length
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

        ; THE PARENT THROUGH FS, NOT THROUGH DS.  This walked the
        ; parent's table with DS pointing at the parent's PSP - and
        ; then called sft_addref, which reaches RMDOS's own handle
        ; table through DS.  So the reference count it incremented was
        ; a word in the PARENT's memory, and the count that should
        ; have gone up did not: five inherited handles, none of them
        ; actually held.  The 386 has two more segment registers than
        ; the code that has to be compatible with them; this is what
        ; they are for.
        ; ...and the parent's table is wherever 67h left it, which is
        ; not necessarily its PSP.  The child's always starts in its
        ; own PSP: a fresh process gets the twenty DOS builds, and asks
        ; for more itself if it wants them.
        push    es
        push    di
        mov     es,[rd_psp]
        call    jft_ptr                 ; -> ES:DI the parent's table
        mov     si,di
        mov     ax,es
        pop     di
        pop     es
        mov     fs,ax
        mov     di,18h
        mov     cx,5
ep_inherit:
        mov     al,fs:[si]
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

        ; The environment and the command tail, out of the parameter
        ; block the caller passed.
        mov     es,[bp+F_ES]
        mov     bx,[bp+F_BX]
        mov     ax,es:[bx]                      ; the environment segment
        or      ax,ax
        jnz     ep_haveenv
        push    es
        mov     es,[rd_psp]                     ; 0 means the CALLER'S OWN,
        mov     ax,es:[2Ch]                     ; which is what DOS uses
        pop     es
ep_haveenv:
        call    ep_envcopy                      ; and the child gets a COPY
IFDEF DBG_LPT
        call    rdbg_env                ; what the child is handed, verbatim
ENDIF
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

        ; The command tail, all 128 bytes of it, to PSP:80h.
        ;
        ; EVERY ARGUMENT IS FETCHED BEFORE THE LDS, and that is the
        ; whole of this routine's history.  It used to load DS:SI from
        ; ep_tail and THEN read [rd_execpsp] for the destination - but
        ; rd_execpsp is a variable in RMDOS's segment and DS was no
        ; longer RMDOS's segment, so the destination was whatever
        ; happened to lie at that offset in the CALLER's.  It lay at
        ; zero, so a hundred and twenty-eight bytes of command tail
        ; went to 0000:0080, which is the INTERRUPT VECTOR TABLE from
        ; INT 20h to INT 3Fh.  INT 21h's vector became bytes 4 to 7 of
        ; the tail, and the child - already loaded, already jumped to,
        ; already running - died on its first DOS call, with the
        ; loader having reported success.
        ;
        ; LDS IS AN ASSIGNMENT TO DS.  Nothing after it may name a
        ; variable of ours until DS is ours again.
        push    ds
        mov     ax,[rd_execpsp]         ; while DS is still this segment
        mov     es,ax
        mov     di,80h
        mov     cx,80h
        lds     si,dword ptr [ep_tail]  ; ...and only now let go of it
        cld
        rep     movsb
        pop     ds

        pop     ds
        pop     es
        popa
        clc
        ret


;=====================================================================
; ep_envcopy - the child's environment, which is a COPY with the
;              child's own path on the end of it.
;
; AX = the block to copy from, 0 for none.  AX = the child's block, or
; 0.  Everything else preserved.
;
; THIS IS WHAT STOPPED WINDOWS, and it is worth saying exactly how.  A
; DOS environment is the strings back to back, an empty string to end
; them, and then - since DOS 3.0 - the word 0001h and the FULL PATH OF
; THE PROGRAM ITSELF.  That tail is not decoration and it is not for
; the C runtime's benefit alone: KRNL386 walks past the strings, reads
; the count and OPENS THE PATH IT FINDS THERE, because that is how the
; Windows kernel finds its own file to load its own segments from.
;
; RMDOS used to hand the child whatever segment the caller put in the
; parameter block and nothing at all when that was zero - and zero is
; what the extender passes, because zero is how a program says "give
; it a copy of mine".  So the kernel was handed no environment, found
; no path, printed "KERNEL: Unable to open KERNEL executable" and
; exited, and NO OPEN EVER REACHED THIS DOS to say what it had wanted.
; A trace of DOS calls cannot show a call that was never made; what
; showed it was recording the environment itself, at the moment the
; child was handed it.
;
; MSDOS 6's EXEC is the specification here and it is followed to the
; letter (msproc.asm, Exec_Check_Environ): the parameter block first,
; the caller's PDB_environ if that is zero, nothing if that is zero
; too - and then always a fresh block of
;
;       (length + 0Fh + 2 + name length) >> 4
;
; paragraphs, the strings copied, the word 0001h, the name.  DOS makes
; a copy even when it was given a block outright, which is why SET in
; a child never reaches its parent.
;
; The block is stamped with the CHILD's PSP, not the caller's, for the
; same reason the image is: mem_alloc stamps whoever is running, and a
; block owned by the parent is a block the child's termination will
; not free.
;=====================================================================
ep_envcopy:
        or      ax,ax
        jz      ec_out
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        mov     [ep_envsrc],ax

;--- how long are the strings, the empty one that ends them included?
        mov     es,ax
        xor     di,di
        xor     cx,cx
ec_scan:
        cmp     byte ptr es:[di],0              ; an empty string ends them
        je      ec_sized
ec_ch:
        inc     di
        inc     cx
        cmp     cx,8000h                        ; a runaway block is not a
        jae     ec_fail                         ; block
        cmp     byte ptr es:[di],0
        jne     ec_ch
        inc     di
        inc     cx
        jmp     short ec_scan
ec_sized:
        inc     cx                              ; the empty string itself
        mov     [ep_envlen],cx

;--- and how long is the name the child is being started by?
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        xor     dx,dx
ec_nlen:
        cmp     byte ptr es:[di],0
        je      ec_named
        inc     di
        inc     dx
        cmp     dx,80h
        jb      ec_nlen
        jmp     short ec_fail
ec_named:
        inc     dx                              ; its own terminator
        mov     [ep_envnam],dx

;--- room for the two of them and the word between
        mov     bx,cx
        add     bx,dx
        add     bx,2
        add     bx,15
        shr     bx,4
        call    mem_alloc
        jc      ec_fail
        mov     [ep_envnew],ax

        mov     bx,ax                           ; and it is the CHILD's
        dec     bx
        mov     es,bx
        mov     bx,[rd_execpsp]
        mov     es:[RMCB_OWN],bx

;--- the strings, the count, the path
        mov     es,ax
        xor     di,di
        mov     ds,[ep_envsrc]                  ; read while DS is still ours
        xor     si,si
        mov     cx,word ptr cs:[ep_envlen]
        cld
        rep     movsb
        mov     ax,1                            ; how many things follow
        stosw
        mov     ds,[bp+F_DS]                    ; the frame is on SS
        mov     si,[bp+F_DX]
        mov     cx,word ptr cs:[ep_envnam]
        rep     movsb
        push    cs
        pop     ds
        mov     ax,[ep_envnew]
        jmp     short ec_done
ec_fail:
        push    cs
        pop     ds
        xor     ax,ax
ec_done:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
ec_out:
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
rd_exechigh db  0
rd_execseg  dw  0
rd_execcs   dw  0
rd_execip   dw  0
rd_execjmp  dd  0                   ; the child's CS:IP, jumped through
rd_execss   dw  0
rd_execsp   dw  0
rd_execsize dd  0
rd_exechdrsz dd 0
rd_execparas dd 0
rd_execleft dd  0
rd_exehdr   db  32 dup(0)
rd_relbuf   db  4 dup(0)
ep_tail     dd  0
ep_envsrc   dw  0
ep_envnew   dw  0
ep_envlen   dw  0
ep_envnam   dw  0
