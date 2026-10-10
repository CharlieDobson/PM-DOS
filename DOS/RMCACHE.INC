;=====================================================================
; RMCACHE.INC - the sector cache RMDOS keeps while it is the DOS
;
; PM-DOS's own cache (CACHE.INC) gives its memory back before the park
; and is not there to be asked while Windows runs, so every read
; Windows made of the DOS underneath it - KRNL386 loading a segment it
; discarded a minute ago, Program Manager reading a .GRP, a DOS box
; running COMMAND.COM - went to the BIOS, a sector at a time.  On a
; real DOS that is SMARTDRV's job, and this is SMARTDRV's job done
; here: a cache of the sectors RMDOS moves, in memory nothing else can
; want for the length of the excursion, with SMARTDRV's own INT 2Fh
; 4A10h interface in front of it so that the programs that ask after
; SMARTDRV - SMARTMON, the Windows for Workgroups file system, an
; installer wanting the cache flushed - get the answers they expect.
; The interface is the one in DOS6SRC dev\smartdrv\int2f.asm, which
; is SMARTDRV 4.10 and the version BX=0 reports.
;
;---------------------------------------------------------------------
; WHERE THE MEMORY IS, AND HOW IT IS REACHED.
;
; Out of the XMS pool RMXMS.INC was handed, allocated through the
; driver's own entry like any client would (cm_init), so that the
; pool's accounting keeps Windows off it: WIN386 asks the driver how
; much is free and takes all of it, and what this file holds is not
; free.  The block is locked, which is to say its physical address is
; known, and sectors are moved between it and conventional memory by
; the driver's own mover (rx_domove) while the machine is in real mode
; - standard mode, and the real-mode part of a WIN386 start - and by
; INT 15h AH=87h once it is not (rx_move15): under WIN386 the DOS is
; inside a virtual 8086 machine, CR0 belongs to somebody else, and the
; monitor's emulation of 87h is the one mover that works from there.
; That it works is PROVED, not assumed.  The first transfer that finds
; the machine in protected mode writes a pattern out to a spare
; element and reads it back (cm_selftest), and a mover that does not
; round-trip 512 bytes turns the cache off before any data has gone
; through it.
;
; NOT through the XMS entry under Windows, deliberately.  V86MMGR
; takes that entry over, and what it does with a handle it did not
; issue - this one is older than Windows - is its business and not a
; thing to depend on.  87h is documented, every V86 monitor Microsoft
; shipped emulates it (EMM386's int15.asm says so in as many words),
; and it needs nobody's handle.
;
;---------------------------------------------------------------------
; WHAT IT HOLDS.
;
; Elements of eight sectors, 4K, keyed by the BIOS drive and the
; absolute LBA - the kernel cache's block and the kernel cache's key,
; for the kernel cache's reason: several letters share a disk, and
; INT 25h addresses a disk.  Each element has a bit per sector, so an
; element may be partly held and a write of one sector fills in one
; bit.  A read that misses and asks for less than an element reads
; the WHOLE element into the bounce buffer, which is the read-ahead:
; RMFILE.INC walks the FAT two sectors at a time and a directory one
; sector at a time, and the seventh sector of a directory is wanted
; right after the first.  A read of a whole aligned element, or more,
; goes straight into the caller's buffer and is kept from there.
;
; WRITE-THROUGH, ALWAYS.  The disk is written first and the cache
; brought up to date afterwards, so a cache that is turned off, lost
; or wrong can cost speed and never data: Windows is the program on
; top, Windows ends in ways a DOS does not get to object to, and the
; kernel's lazy writer lives on the other side of the park, where a
; prompt can flush it.  Nothing is ever owed, BX=1 has nothing to
; commit, and BX=0 reports no dirty elements.
;
; Floppies are not cached - a disk can change between two reads and
; RMDOS has no change line, which is why rd_getvol re-mounts one every
; time - and nor is any drive a BX=3 has turned off.  The WfW 3.11
; file system does that for each drive it takes over, because from
; then on it reads and writes the disk behind the DOS's back, and a
; cache that remembered what the DOS last saw would describe a disk
; that had moved on.  Turning a drive off drops what was held for it.
;
;---------------------------------------------------------------------
; HOW BIG, AND WHY THAT IS THE KERNEL'S DECISION.
;
; RMH_WINCACHE says, in KB.  PARK.INC fills it from WINCACHE= in
; CONFIG.SYS, or from an eighth of the pool when the line is absent,
; and 0 means none.  SMARTDRV took two sizes, one for DOS and one for
; Windows, and shrank to the second on INT 2Fh 1605h - the broadcast
; this file never sees, because 1605h is what parks PM-DOS and this
; file comes into existence AFTER it.  So there is one size, the
; Windows one, and the shrink SMARTDRV did at 1605h is done here by
; never being bigger.  The directory names CM_MAXEL elements and the
; kernel caps the request to match.  The directory, the chain heads
; and the bounce buffer are in this image, which has the room - the
; segment is 64K and the image was 37K - and so cost no conventional
; memory beyond what the park already takes.
;
;---------------------------------------------------------------------
; REPLACEMENT is the clock: a reference bit an element gets when it is
; used, a hand that clears bits as it passes and takes the first
; element it finds clear.  Fresh entries are handed out in order
; before the hand turns at all.  The lookup is a hash of the element's
; first sector and its disk into CM_HASHN chains threaded through the
; directory entries themselves; entries are named by their OFFSET in
; the directory throughout, and CM_NONE ends a chain.
;
; A 4A10h from one virtual machine can land while another is inside a
; DOS call down here - DOSMGR serialises INT 21h, not INT 2Fh - and
; the worst that does is drop an entry the call was about to fill,
; which costs the fill.  Nothing here is read after it is freed except
; a slot's bytes, which are still the bytes they were.
;
; Included by RMDOS.ASM after RMXMS.INC.  Real mode throughout; CS =
; DS = SS as everywhere in this image.
;=====================================================================

CM_ESECS        EQU 8                   ; sectors an element holds
CM_EBYTES       EQU CM_ESECS * SECSIZE  ; 4K
CM_MAXEL        EQU 1024                ; the most elements: 4MB
CM_HASHN        EQU 1024                ; chain heads, a power of two
CM_NONE         EQU 0FFFFh              ; no entry; the end of a chain
CM_LOGN       EQU 32                  ; 4A10h calls the log remembers
CM_SIG          EQU 0BABEh              ; "BAMBI is here"
CM_VERSION      EQU 0410h               ; ...and which one

; A directory entry
CE_LBA          EQU 0                   ; dd  the element's first sector
CE_BIOS         EQU 4                   ; db  BIOS drive; FFh = a free entry
CE_VALID        EQU 5                   ; db  a bit per sector that is held
CE_FLAGS        EQU 6                   ; db  CEF_*
CE_SPARE        EQU 7                   ; db
CE_NEXT         EQU 8                   ; dw  next on the chain, CM_NONE
CE_LEN          EQU 10

CEF_REF         EQU 01h                 ; used since the hand went by

;=====================================================================
; cm_init - take the memory and start empty.  Called from rd_entry
;           after rx_init, and BEFORE the IRET into Windows' loader,
;           which is the next thing to ask the pool what is free.
;=====================================================================
cm_init:
        pusha
        push    es
        mov     byte ptr [cmv_on],0
        mov     byte ptr [cmv_tested],0
        mov     byte ptr [cmv_mover],0
        mov     word ptr [cmv_nel],0
        mov     ax,[h_wincache]
        mov     [cmv_kb],ax
        mov     ax,[h_wcflags]
        mov     [cmv_flags],ax
        mov     ax,[cmv_kb]
        shr     ax,2                    ; KB -> elements of 4K
        jz      cmi_out
        cmp     ax,CM_MAXEL
        jbe     cmi_sized
        mov     ax,CM_MAXEL
cmi_sized:
        mov     [cmv_nel],ax
        imul    ax,ax,CE_LEN
        add     ax,OFFSET cmv_dir
        mov     [cmv_dirend],ax

        ; One element more than the directory names: the self-test's.
        ; PUSH CS / CALL, because the entry ends in a RETF.
        mov     dx,[cmv_nel]
        inc     dx
        shl     dx,2                    ; elements -> KB
        mov     ah,09h
        push    cs
        call    rx_entry
        or      ax,ax
        jz      cmi_nomem
        mov     [cmv_handle],dx
        mov     ah,0Ch                  ; lock: where is it, really?
        push    cs
        call    rx_entry
        or      ax,ax
        jz      cmi_nomem               ; (the handle is leaked; nothing
                                        ; will ever use it)
        mov     word ptr [cmv_base],bx
        mov     word ptr [cmv_base+2],dx
        call    cm_dropall
        mov     byte ptr [cmv_on],1
cmi_out:
        pop     es
        popa
        ret
cmi_nomem:
        mov     word ptr [cmv_nel],0
        jmp     short cmi_out

;=====================================================================
; cm_xfer - rd_xfer's one call: EAX = first sector, ECX = how many,
;           ES:BX = the buffer, SI = the volume, [rd_rw] and the rd_*
;           copies of the volume set.  CF on error.
;
; The decisions that bypass the cache are all here, so that the read
; and write paths can assume they are wanted.
;=====================================================================
cm_xfer:
        cmp     byte ptr [cmv_on],0
        je      cmx_raw
        test    byte ptr [rd_vflags],VF_FLOPPY OR VF_DRIVER
        jnz     cmx_raw                 ; a driver's unit is its business:
                                        ; RAMDRIVE is memory already
        cmp     byte ptr [rd_bios],80h
        jb      cmx_raw

        ; Under a monitor the mover is INT 15h's, and it is proved once
        push    ax
        smsw    ax
        test    al,1
        pop     ax
        jz      cmx_mode
        cmp     byte ptr [cmv_tested],0
        jne     cmx_mode
        call    cm_selftest
        jc      cmx_raw
cmx_mode:
        cmp     byte ptr [rd_rw],0
        jne     cm_write
        test    byte ptr [rd_vflags],VF_NOREAD
        jnz     cmx_raw
        jmp     cm_read
cmx_raw:
        mov     byte ptr [cmv_last],'.'
        jmp     rd_raw

;=====================================================================
; cm_read - the registers of cm_xfer.  Each piece of the request that
;           lies in one element is answered from the element if every
;           sector of it is there, and read otherwise.
;=====================================================================
cm_read:
        pushad
        mov     byte ptr [cmv_allhit],1
cmr_loop:
        or      ecx,ecx
        jz      cmr_done
        call    cm_piece                ; -> [cmv_sub], [cmv_n]
        call    cm_find                 ; -> DI, or CM_NONE
        jc      cmr_miss
        call    cm_mask                 ; DL = the sectors wanted
        mov     dh,[di+CE_VALID]
        not     dh                      ; DH = the sectors NOT held
        test    dh,dl                   ; any of them wanted?
        jnz     cmr_miss
        call    cm_fromcache
        jc      cmr_dead                ; the mover failed: no cache now
        or      byte ptr [di+CE_FLAGS],CEF_REF
        jmp     short cmr_next
cmr_dead:
        call    cm_off
cmr_miss:
        mov     byte ptr [cmv_allhit],0
        mov     [cmv_ent],di
        call    cm_readmiss
        jc      cmr_fail
cmr_next:
        call    cm_advance
        jmp     cmr_loop
cmr_done:
        mov     byte ptr [cmv_last],'H'
        cmp     byte ptr [cmv_allhit],0
        jne     cmr_hit
        mov     byte ptr [cmv_last],'M'
        add     dword ptr [cmv_miss],1
        jmp     short cmr_out
cmr_hit:
        add     dword ptr [cmv_hits],1
cmr_out:
        popad
        clc
        ret
cmr_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; cm_readmiss - the piece [cmv_sub]/[cmv_n] at EAX is not held: get it
;               into ES:BX, and into the cache if the cache is still
;               on.  [cmv_ent] = the entry it belongs in, or CM_NONE.
;               SI = the volume, whose end bounds the read-ahead.
;---------------------------------------------------------------------
cm_readmiss:
        pushad
        cmp     byte ptr [cmv_on],0
        je      cmq_rawonly
        cmp     byte ptr [cmv_nora],0
        jne     cmq_direct              ; the bounce is in use (cm_winexit)
        cmp     word ptr [cmv_n],CM_ESECS
        je      cmq_direct              ; a whole element: no bounce needed
        cmp     dword ptr [si+VOL_TOTSEC],0
        je      cmq_direct              ; not mounted yet: no end to clip at

        ; The whole element, clipped at the end of the volume
        mov     edx,eax
        and     edx,NOT (CM_ESECS - 1)
        mov     [cmv_elba],edx
        mov     ecx,dword ptr [si+VOL_START]
        add     ecx,dword ptr [si+VOL_TOTSEC]
        sub     ecx,edx                 ; sectors from the element to the end
        jbe     cmq_direct
        cmp     ecx,CM_ESECS
        jbe     cmq_cnt
        mov     ecx,CM_ESECS
cmq_cnt:
        mov     [cmv_ecnt],cx
        mov     dx,[cmv_sub]
        add     dx,[cmv_n]
        cmp     dx,cx
        ja      cmq_direct              ; the piece runs past the volume

        push    eax
        push    es
        push    bx
        push    cs
        pop     es
        mov     bx,OFFSET cmv_bounce
        mov     eax,[cmv_elba]
        call    rd_raw
        pop     bx
        pop     es
        pop     eax
        jc      cmq_direct              ; something in it is bad: ask only
                                        ; for what was wanted
        ; The piece, from the bounce to the caller
        mov     si,[cmv_sub]
        shl     si,9
        add     si,OFFSET cmv_bounce
        mov     di,bx
        mov     cx,[cmv_n]
        shl     cx,7                    ; sectors -> dwords
        cld
        rep     movsd

        ; ...and the whole element into the cache, from the bounce
        push    word ptr [cmv_sub]
        push    word ptr [cmv_n]
        mov     word ptr [cmv_sub],0
        mov     ax,[cmv_ecnt]
        mov     [cmv_n],ax
        mov     ax,cs
        movzx   esi,ax
        shl     esi,4
        add     esi,OFFSET cmv_bounce
        mov     eax,[cmv_elba]
        mov     di,[cmv_ent]
        call    cm_fill
        pop     word ptr [cmv_n]
        pop     word ptr [cmv_sub]
        jmp     short cmq_ok

cmq_direct:
        movzx   ecx,word ptr [cmv_n]
        call    rd_raw
        jc      cmq_fail
        cmp     byte ptr [cmv_on],0
        je      cmq_ok
        call    cm_flat                 ; ES:BX -> EDI
        mov     esi,edi
        mov     di,[cmv_ent]
        call    cm_fill
        jmp     short cmq_ok

cmq_rawonly:
        movzx   ecx,word ptr [cmv_n]
        call    rd_raw
        jc      cmq_fail
cmq_ok:
        popad
        clc
        ret
cmq_fail:
        popad
        stc
        ret

;=====================================================================
; cm_write - the registers of cm_xfer.  The disk first; then what was
;            written goes into the cache, or out of it for a drive
;            that is not being cached.
;=====================================================================
cm_write:
        pushad
        call    rd_raw
        jc      cmw_fail
        cmp     byte ptr [cmv_on],0
        je      cmw_ok
        test    byte ptr [rd_vflags],VF_NOREAD OR VF_NOWRITE
        jnz     cmw_drop
cmw_loop:
        or      ecx,ecx
        jz      cmw_ok
        call    cm_piece
        call    cm_find
        mov     [cmv_ent],di
        call    cm_flat                 ; ES:BX -> EDI
        mov     esi,edi
        mov     di,[cmv_ent]
        call    cm_fill
        cmp     byte ptr [cmv_on],0     ; a fill that failed turned it off
        je      cmw_ok
        call    cm_advance
        jmp     cmw_loop
cmw_drop:
        call    cm_droprange
cmw_ok:
        mov     byte ptr [cmv_last],'W'
        popad
        clc
        ret
cmw_fail:
        cmp     byte ptr [cmv_on],0
        je      cmw_failout
        call    cm_droprange            ; what the disk holds now is
cmw_failout:                            ; anybody's guess
        popad
        stc
        ret

;---------------------------------------------------------------------
; cm_droprange - EAX, ECX: forget whatever is held of those sectors.
;---------------------------------------------------------------------
cm_droprange:
        pushad
cdr_loop:
        or      ecx,ecx
        jz      cdr_out
        call    cm_piece
        call    cm_find
        jc      cdr_next
        call    cm_mask
        not     dl
        and     byte ptr [di+CE_VALID],dl
        jnz     cdr_next
        call    cm_unlink               ; nothing left in it
        mov     byte ptr [di+CE_BIOS],0FFh
cdr_next:
        call    cm_advance
        jmp     cdr_loop
cdr_out:
        popad
        ret

;=====================================================================
; The pieces.  A request is walked an element at a time: cm_piece
; works out how much of what is left lies in the element EAX is in,
; cm_advance moves EAX, ECX and ES:BX past it.
;=====================================================================

;--- cm_piece - EAX, ECX -> [cmv_sub] = which sector of the element,
;--- [cmv_n] = how many of the request's sectors lie in it
cm_piece:
        push    edi
        mov     di,ax
        and     di,CM_ESECS - 1
        mov     [cmv_sub],di
        neg     di
        add     di,CM_ESECS
        movzx   edi,di
        cmp     edi,ecx
        jbe     cpc_fits
        mov     edi,ecx
cpc_fits:
        mov     [cmv_n],di
        pop     edi
        ret

;--- cm_advance - past the piece: EAX, ECX and ES:BX, with ES carried
;--- when BX wraps, which is what a 32K transfer does
cm_advance:
        push    edi
        movzx   edi,word ptr [cmv_n]
        add     eax,edi
        sub     ecx,edi
        shl     di,9
        add     bx,di
        jnc     cad_out
        mov     di,es
        add     di,1000h
        mov     es,di
cad_out:
        pop     edi
        ret

;--- cm_mask - [cmv_sub], [cmv_n] -> DL = a bit for each sector of the
;--- piece, DH = 0.  Not AL: EAX is the LBA in every caller.
cm_mask:
        push    cx
        mov     cx,[cmv_n]
        mov     dx,1
        shl     dx,cl
        dec     dx
        mov     cx,[cmv_sub]
        shl     dl,cl
        pop     cx
        ret

;=====================================================================
; The directory.
;=====================================================================

;--- cm_hash - EAX = an element's first sector, DL = its BIOS drive
;--- -> BX -> the chain head.  EAX and EDX kept.
cm_hash:
        push    eax
        push    edx
        mov     ebx,eax
        shr     eax,3
        shr     ebx,13
        xor     eax,ebx
        movzx   ebx,dl
        shl     ebx,4
        xor     eax,ebx
        and     ax,CM_HASHN - 1
        shl     ax,1
        add     ax,OFFSET cmv_hash
        mov     bx,ax
        pop     edx
        pop     eax
        ret

;--- cm_find - EAX = any sector, [rd_bios] = the disk -> CF clear and
;--- DI = the entry holding its element, or CF set and DI = CM_NONE.
;--- BX kept: it is the caller's buffer.
cm_find:
        push    eax
        push    bx
        push    dx
        push    si
        and     eax,NOT (CM_ESECS - 1)
        mov     dl,[rd_bios]
        call    cm_hash
        mov     si,[bx]
cmf_walk:
        cmp     si,CM_NONE
        je      cmf_none
        cmp     dword ptr [si+CE_LBA],eax
        jne     cmf_next
        cmp     [si+CE_BIOS],dl
        je      cmf_found
cmf_next:
        mov     si,[si+CE_NEXT]
        jmp     short cmf_walk
cmf_found:
        mov     di,si
        pop     si
        pop     dx
        pop     bx
        pop     eax
        clc
        ret
cmf_none:
        mov     di,CM_NONE
        pop     si
        pop     dx
        pop     bx
        pop     eax
        stc
        ret

;--- cm_link - DI = an entry with its LBA and drive filled in: onto
;--- the front of its chain
cm_link:
        push    eax
        push    bx
        push    dx
        mov     eax,dword ptr [di+CE_LBA]
        mov     dl,[di+CE_BIOS]
        call    cm_hash
        mov     ax,[bx]
        mov     [di+CE_NEXT],ax
        mov     [bx],di
        pop     dx
        pop     bx
        pop     eax
        ret

;--- cm_unlink - DI = an entry: off its chain, whatever else is in it
cm_unlink:
        push    eax
        push    bx
        push    dx
        push    si
        mov     eax,dword ptr [di+CE_LBA]
        mov     dl,[di+CE_BIOS]
        call    cm_hash
cmu_walk:
        mov     si,[bx]
        cmp     si,CM_NONE
        je      cmu_out                 ; not on it: nothing to do
        cmp     si,di
        je      cmu_got
        lea     bx,[si+CE_NEXT]
        jmp     short cmu_walk
cmu_got:
        mov     ax,[di+CE_NEXT]
        mov     [bx],ax
        mov     word ptr [di+CE_NEXT],CM_NONE
cmu_out:
        pop     si
        pop     dx
        pop     bx
        pop     eax
        ret

;--- cm_take -> DI = an entry nobody holds, off every chain.  Fresh
;--- ones first; then the clock.
cm_take:
        push    ax
        push    si
        mov     ax,[cmv_used]
        cmp     ax,[cmv_nel]
        jae     cmt_clock
        inc     word ptr [cmv_used]
        imul    ax,ax,CE_LEN
        add     ax,OFFSET cmv_dir
        mov     di,ax
        jmp     short cmt_out
cmt_clock:
        mov     si,[cmv_hand]
cmt_turn:
        mov     di,si
        add     si,CE_LEN
        cmp     si,[cmv_dirend]
        jb      cmt_nowrap
        mov     si,OFFSET cmv_dir
cmt_nowrap:
        cmp     byte ptr [di+CE_BIOS],0FFh
        je      cmt_free                ; dropped earlier: nothing to unlink
        test    byte ptr [di+CE_FLAGS],CEF_REF
        jz      cmt_victim
        and     byte ptr [di+CE_FLAGS],NOT CEF_REF
        jmp     short cmt_turn          ; ends: every bit it clears, it
                                        ; takes on the next pass
cmt_victim:
        call    cm_unlink
cmt_free:
        mov     [cmv_hand],si
cmt_out:
        pop     si
        pop     ax
        ret

;--- cm_slot - DI = an entry -> EAX = the flat address of its element
cm_slot:
        push    bx
        push    dx
        movzx   eax,di
        sub     ax,OFFSET cmv_dir
        xor     dx,dx
        mov     bx,CE_LEN
        div     bx                      ; AX = the index
        shl     eax,12                  ; * 4K
        add     eax,[cmv_base]
        pop     dx
        pop     bx
        ret

;--- cm_flat - ES:BX -> EDI, flat
cm_flat:
        push    eax
        mov     ax,es
        movzx   edi,ax
        shl     edi,4
        movzx   eax,bx
        add     edi,eax
        pop     eax
        ret

;--- cm_dropall - start empty.  Also BX=2, and what cm_init calls.
cm_dropall:
        pusha
        push    es
        push    cs
        pop     es
        mov     di,OFFSET cmv_hash
        mov     cx,CM_HASHN
        mov     ax,CM_NONE
        cld
        rep     stosw
        mov     di,OFFSET cmv_dir
        mov     cx,CM_MAXEL * CE_LEN / 2
        rep     stosw                   ; every drive byte FFh: all free
        mov     word ptr [cmv_used],0
        mov     word ptr [cmv_hand],OFFSET cmv_dir
        pop     es
        popa
        ret

;--- cm_dropdrive - AL = a BIOS drive: forget everything held for it
cm_dropdrive:
        pusha
        mov     di,OFFSET cmv_dir
        mov     cx,[cmv_used]
        jcxz    cmd_out
cmd_one:
        cmp     [di+CE_BIOS],al
        jne     cmd_next
        call    cm_unlink
        mov     byte ptr [di+CE_BIOS],0FFh
cmd_next:
        add     di,CE_LEN
        loop    cmd_one
cmd_out:
        popa
        ret

;--- cm_off - no more caching; what is held is simply never consulted
cm_off:
        mov     byte ptr [cmv_on],0
        ret

;=====================================================================
; Moving the bytes.
;=====================================================================

;--- cm_move - ESI -> EDI, ECX bytes, flat both.  CF if it failed.
;--- Real mode: ours.  Under a monitor: whichever of the two movers
;--- cm_selftest settled on - the XMS entry (V86MMGR's, under WIN386,
;--- which can do the move in ring 0) or INT 15h 87h.
cm_move:
        push    ax
        smsw    ax
        test    al,1
        jnz     cmm_v86
        call    rx_domove               ; ours: leave real mode and back
        jmp     short cmm_out
cmm_v86:
        cmp     byte ptr [cmv_mover],2
        je      cmm_xms
        call    rx_move15               ; the monitor's 87h
        jmp     short cmm_out
cmm_xms:
        call    cm_xmove                ; the monitor's XMS
cmm_out:
        pop     ax
        ret

;--- cm_xmove - the same move as an XMS 0Bh through the driver's entry,
;--- one side of it in the cache's own block: the entry is ours in real
;--- mode and V86MMGR's breakpoint under WIN386, and a handle older
;--- than Windows is either known to it or handed back down to us,
;--- which is still a move.  CF if the call said no.
cm_xmove:
        pushad
        push    ds
        push    es
        mov     dword ptr [cmv_xdesc+XM_LEN],ecx
        mov     eax,[cmv_base]
        cmp     esi,eax
        jb      cxm_fromlow
        mov     ax,[cmv_handle]         ; the source is the block
        mov     word ptr [cmv_xdesc+XM_SRCH],ax
        mov     eax,esi
        sub     eax,[cmv_base]
        mov     dword ptr [cmv_xdesc+XM_SRCO],eax
        mov     word ptr [cmv_xdesc+XM_DSTH],0
        mov     eax,edi
        call    cm_segoff
        mov     dword ptr [cmv_xdesc+XM_DSTO],eax
        jmp     short cxm_go
cxm_fromlow:
        mov     word ptr [cmv_xdesc+XM_SRCH],0
        mov     eax,esi
        call    cm_segoff
        mov     dword ptr [cmv_xdesc+XM_SRCO],eax
        mov     ax,[cmv_handle]         ; the destination is the block
        mov     word ptr [cmv_xdesc+XM_DSTH],ax
        mov     eax,edi
        sub     eax,[cmv_base]
        mov     dword ptr [cmv_xdesc+XM_DSTO],eax
cxm_go:
        mov     ax,cs
        mov     ds,ax
        mov     si,OFFSET cmv_xdesc
        mov     ah,0Bh
        push    cs
        call    rx_entry
        pop     es
        pop     ds
        or      ax,ax
        jz      cxm_bad
        popad
        clc
        ret
cxm_bad:
        popad
        stc
        ret

;--- cm_segoff - EAX = a flat address below the megabyte -> EAX = the
;--- same as SEGMENT:OFFSET in one dword, the way XMS handle 0 wants it
cm_segoff:
        push    edx
        mov     edx,eax
        shr     edx,4
        shl     edx,16
        and     eax,0Fh
        or      eax,edx
        pop     edx
        ret

;--- cm_fromcache - DI = an entry: its sectors [cmv_sub] for [cmv_n]
;--- to ES:BX.  CF if the mover failed.
cm_fromcache:
        pushad
        call    cm_slot                 ; EAX = the element
        movzx   ecx,word ptr [cmv_sub]
        shl     ecx,9
        add     eax,ecx
        mov     esi,eax
        call    cm_flat                 ; EDI = the caller's buffer
        movzx   ecx,word ptr [cmv_n]
        shl     ecx,9
        call    cm_move
        popad
        ret

;--- cm_fill - EAX = the first sector being kept, ESI = its bytes
;--- (flat), [cmv_sub]/[cmv_n] = which sectors of the element they
;--- are, DI = the entry, or CM_NONE for one to be taken.  Cannot fail
;--- as far as the caller is concerned: a mover that fails turns the
;--- cache off instead, and the caller already has its data.
cm_fill:
        pushad
        cmp     di,CM_NONE
        jne     cmg_have
        call    cm_take
        and     eax,NOT (CM_ESECS - 1)
        mov     dword ptr [di+CE_LBA],eax
        mov     dl,[rd_bios]
        mov     [di+CE_BIOS],dl
        mov     byte ptr [di+CE_VALID],0
        mov     byte ptr [di+CE_FLAGS],0
        call    cm_link
cmg_have:
        call    cm_slot                 ; EAX = the element
        movzx   ecx,word ptr [cmv_sub]
        shl     ecx,9
        add     eax,ecx
        push    di
        mov     edi,eax
        movzx   ecx,word ptr [cmv_n]
        shl     ecx,9
        call    cm_move
        pop     di
        jc      cmg_bad
        call    cm_mask
        or      [di+CE_VALID],dl
        or      byte ptr [di+CE_FLAGS],CEF_REF
        popad
        ret
cmg_bad:
        call    cm_off
        popad
        ret

;---------------------------------------------------------------------
; cm_selftest - the mover, under a monitor, proved on the spare
;               element before anything depends on it: the XMS entry
;               first, because under WIN386 that is V86MMGR and a move
;               it does itself is a ring-0 copy, then INT 15h 87h.  The
;               first that round-trips 512 bytes is the mover; neither,
;               and the cache is off.  CF says so.  Once only.
;---------------------------------------------------------------------
cm_selftest:
        pushad
        push    es
        mov     byte ptr [cmv_tested],1
        movzx   eax,word ptr [cmv_nel]
        shl     eax,12
        add     eax,[cmv_base]          ; the spare element
        mov     [cmv_elba],eax          ; (a flat address, borrowing the field)
        ; NOT THE XMS ENTRY, and this is the test that found out why.
        ; With mover 2 tried first the round trip PASSED and Windows
        ; never got past its logo: V86MMGR took the handle for one of
        ; its own, moved the pattern into somebody else's memory and
        ; read it back from there, which a round trip cannot tell from
        ; the truth.  A handle issued before Windows is only safe with
        ; V86MMGR when it can find the driver's table (INT 2Fh 4309h),
        ; which RMXMS does not publish.  So 87h, which the monitor
        ; emulates against the physical address it is given.
        mov     byte ptr [cmv_mover],1
        call    cm_roundtrip
        jnc     cms_ok
        call    cm_off
        pop     es
        popad
        stc
        ret
cms_ok:
        pop     es
        popad
        clc
        ret

;--- cm_roundtrip - a pattern out to the spare element with the mover
;--- [cmv_mover] names, back into the other half of the bounce, and
;--- compared.  CF if the mover failed or the bytes differ.
cm_roundtrip:
        pushad
        push    es
        push    cs
        pop     es
        mov     di,OFFSET cmv_bounce
        mov     cx,256
        mov     ax,0A55Ah
        cld
cmt_pat:
        stosw
        inc     ax
        loop    cmt_pat
        mov     cx,256
        xor     ax,ax
        rep     stosw                   ; the second half: where it comes back

        mov     ax,cs
        movzx   esi,ax
        shl     esi,4
        add     esi,OFFSET cmv_bounce
        mov     edi,[cmv_elba]
        mov     ecx,512
        call    cm_move
        jc      cmt_bad
        mov     ax,cs
        movzx   edi,ax
        shl     edi,4
        add     edi,OFFSET cmv_bounce + 512
        mov     esi,[cmv_elba]
        mov     ecx,512
        call    cm_move
        jc      cmt_bad
        mov     si,OFFSET cmv_bounce
        mov     di,OFFSET cmv_bounce + 512
        mov     cx,256
        repe    cmpsw
        jne     cmt_bad
        pop     es
        popad
        clc
        ret
cmt_bad:
        pop     es
        popad
        stc
        ret

;=====================================================================
; cm_api - INT 2Fh AX=4A10h, with the caller's registers and the
;          caller's DS.  The answers are SMARTDRV 4.10's, from its
;          int2f.asm; what it would have chained is left alone, which
;          here means an IRET with nothing changed.
;
;   BX=0  present?  AX=BABEh, DX:BX hits, DI:SI misses, CX dirty (0),
;         BP the version
;   BX=1  commit: nothing is ever owed
;   BX=2  reset: forget everything
;   BX=3  a drive's caching: BP = the letter (0 = A), DL = 0 ask, 1/2
;         read on/off, 3/4 write on/off -> DL = the state (80h no
;         reads, 40h no writes, FFh not a drive this caches)
;   BX=4  sizes: CX bytes an element, AX/BX/DX elements - the full
;         size, the size now and the size under Windows are one here
;=====================================================================
cm_api:
        cmp     word ptr cs:[cmv_nel],0
        je      cma_none                ; no cache was asked for: no SMARTDRV
        push    ds
        push    cs
        pop     ds
        call    cm_apilog
        cmp     bx,0
        je      cma_stats
        cmp     bx,1
        je      cma_done
        cmp     bx,2
        je      cma_reset
        cmp     bx,3
        je      cma_drive
        cmp     bx,4
        je      cma_info
cma_done:
        pop     ds
cma_none:
        iret

cma_stats:
        mov     ax,CM_SIG
        mov     bx,word ptr [cmv_hits]
        mov     dx,word ptr [cmv_hits+2]
        mov     si,word ptr [cmv_miss]
        mov     di,word ptr [cmv_miss+2]
        xor     cx,cx
        mov     bp,CM_VERSION
        jmp     cma_done

cma_reset:
        call    cm_dropall
        jmp     cma_done

cma_info:
        mov     cx,CM_EBYTES
        mov     bx,[cmv_nel]
        mov     ax,bx
        mov     dx,bx
        jmp     cma_done

cma_drive:
        cmp     bp,LASTDRIVE
        jae     cma_nodrive
        push    si
        imul    si,bp,VOL_LEN
        add     si,OFFSET rd_vols
        test    byte ptr [si+VOL_FLAGS],VF_PRESENT
        jz      cma_notours
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jnz     cma_notours
        ; TOLD TO START OR STOP, the drive is marked stale as well: that is
        ; what IFSMGR.386 says round VFAT's use of the disk, which writes
        ; it without this DOS (vol_forget in RMDISK.INC).  DL=0 only asks.
        or      dl,dl
        jz      cma_state
        or      byte ptr [si+VOL_FLAGS],VF_STALE
        cmp     dl,1
        jne     cma_d2
        and     byte ptr [si+VOL_FLAGS],NOT VF_NOREAD AND 0FFh
        jmp     short cma_state
cma_d2:
        cmp     dl,2
        jne     cma_d3
        or      byte ptr [si+VOL_FLAGS],VF_NOREAD
        jmp     short cma_dropit
cma_d3:
        cmp     dl,3
        jne     cma_d4
        and     byte ptr [si+VOL_FLAGS],NOT VF_NOWRITE AND 0FFh
        jmp     short cma_state
cma_d4:
        cmp     dl,4
        jne     cma_state
        or      byte ptr [si+VOL_FLAGS],VF_NOWRITE
cma_dropit:
        push    ax
        mov     al,[si+VOL_BIOS]
        call    cm_dropdrive
        pop     ax
cma_state:
        mov     dl,[si+VOL_FLAGS]
        and     dl,VF_NOREAD OR VF_NOWRITE
        pop     si
        mov     ax,CM_SIG
        jmp     cma_done
cma_notours:
        pop     si
cma_nodrive:
        mov     dl,0FFh
        mov     ax,CM_SIG
        jmp     cma_done

;--- cm_apilog - the question, into the ring the log prints
cm_apilog:
        push    di
        mov     di,[cmv_alogn]
        inc     word ptr [cmv_alogn]
        and     di,CM_LOGN - 1
        imul    di,di,6
        add     di,OFFSET cmv_alog
        mov     [di],bx
        mov     [di+2],dx
        mov     [di+4],bp
        pop     di
        ret

;=====================================================================
; cm_winexit - INT 2Fh AX=1606h: Windows has gone back to real mode
;              and is about to end.  With RMWC_LOG set, RMCACHE.LOG on
;              the boot drive gets the figures and every 4A10h that
;              was made, which is how a file system's use of the
;              interface is read without a debugger.  Written through
;              RMDOS's own INT 21h, like any program would.
;=====================================================================
cm_winexit:
        pusha
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es
        cmp     word ptr [cmv_nel],0
        je      cme_out
        test    word ptr [cmv_flags],RMWC_LOG
        jz      cme_out
        mov     byte ptr [cmv_nora],1   ; the bounce is the text's now
        mov     di,OFFSET cmv_bounce
        mov     si,OFFSET cme_m1
        call    cm_puts
        mov     ax,[cmv_nel]
        call    cm_puthex
        mov     si,OFFSET cme_m2
        call    cm_puts
        mov     ax,[cmv_kb]
        call    cm_puthex
        mov     si,OFFSET cme_m3
        call    cm_puts
        mov     al,[cmv_on]
        call    cm_puthex2
        mov     si,OFFSET cme_m4
        call    cm_puts
        mov     al,[cmv_tested]
        call    cm_puthex2
        mov     si,OFFSET cme_m15
        call    cm_puts
        mov     al,[cmv_mover]
        call    cm_puthex2
        mov     si,OFFSET cme_m5
        call    cm_puts
        mov     eax,[cmv_hits]
        call    cm_puthex32
        mov     si,OFFSET cme_m6
        call    cm_puts
        mov     eax,[cmv_miss]
        call    cm_puthex32
        mov     si,OFFSET cme_m7
        call    cm_puts
        mov     ax,[cmv_used]
        call    cm_puthex
        call    cm_putcrlf

        ; The calls, oldest first
        mov     si,OFFSET cme_m8
        call    cm_puts
        mov     ax,[cmv_alogn]
        call    cm_puthex
        call    cm_putcrlf
        mov     cx,[cmv_alogn]
        cmp     cx,CM_LOGN
        jbe     cme_nall
        mov     cx,CM_LOGN
cme_nall:
        mov     bx,[cmv_alogn]
        sub     bx,cx                   ; the oldest one still there
cme_call:
        jcxz    cme_drives
        push    cx
        mov     ax,bx
        and     ax,CM_LOGN - 1
        imul    ax,ax,6
        add     ax,OFFSET cmv_alog
        mov     si,ax
        push    si
        mov     si,OFFSET cme_m9
        call    cm_puts
        pop     si
        mov     ax,[si]
        call    cm_puthex
        push    si
        mov     si,OFFSET cme_m10
        call    cm_puts
        pop     si
        mov     ax,[si+2]
        call    cm_puthex
        push    si
        mov     si,OFFSET cme_m11
        call    cm_puts
        pop     si
        mov     ax,[si+4]
        call    cm_puthex
        call    cm_putcrlf
        inc     bx
        pop     cx
        loop    cme_call

        ; The drives as they stand
cme_drives:
        xor     cx,cx
        mov     si,OFFSET rd_vols
cme_drv:
        test    byte ptr [si+VOL_FLAGS],VF_PRESENT
        jz      cme_drvnext
        push    si
        mov     si,OFFSET cme_m12
        call    cm_puts
        pop     si
        mov     al,cl
        add     al,'A'
        stosb
        push    si
        mov     si,OFFSET cme_m13
        call    cm_puts
        pop     si
        mov     al,[si+VOL_BIOS]
        call    cm_puthex2
        push    si
        mov     si,OFFSET cme_m14
        call    cm_puts
        pop     si
        mov     al,[si+VOL_FLAGS]
        call    cm_puthex2
        call    cm_putcrlf
cme_drvnext:
        add     si,VOL_LEN
        inc     cx
        cmp     cx,LASTDRIVE
        jb      cme_drv

        ; And out it goes
        mov     al,[h_bootdrv]
        add     al,'A'
        mov     [cmv_logname],al
        mov     ah,3Ch
        xor     cx,cx
        mov     dx,OFFSET cmv_logname
        int     21h
        jc      cme_done
        mov     bx,ax
        mov     cx,di
        sub     cx,OFFSET cmv_bounce
        mov     dx,OFFSET cmv_bounce
        mov     ah,40h
        int     21h
        mov     ah,3Eh
        int     21h
cme_done:
        mov     byte ptr [cmv_nora],0
cme_out:
        pop     es
        pop     ds
        popa
        ret

cme_m1      db  'RMCACHE elements=',0
cme_m2      db  ' kb=',0
cme_m3      db  ' on=',0
cme_m4      db  ' tested=',0
cme_m5      db  ' hits=',0
cme_m6      db  ' misses=',0
cme_m7      db  ' used=',0
cme_m8      db  'API calls=',0
cme_m9      db  ' bx=',0
cme_m10     db  ' dx=',0
cme_m11     db  ' bp=',0
cme_m12     db  'DRV ',0
cme_m13     db  ' bios=',0
cme_m14     db  ' flags=',0
cme_m15     db  ' mover=',0

;--- the text helpers: DS = ES = CS, DI -> where the next byte goes
cm_puts:
        lodsb
        or      al,al
        jz      cmh_out
        stosb
        jmp     short cm_puts
cmh_out:
        ret

cm_puthex32:
        push    eax
        shr     eax,16
        call    cm_puthex
        pop     eax
cm_puthex:
        push    ax
        mov     al,ah
        call    cm_puthex2
        pop     ax
cm_puthex2:
        push    ax
        push    cx
        mov     ah,al
        mov     cx,2
cmh_dig:
        rol     ah,4
        mov     al,ah
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     cmh_put
        add     al,7
cmh_put:
        stosb
        loop    cmh_dig
        pop     cx
        pop     ax
        ret

cm_putcrlf:
        mov     al,13
        stosb
        mov     al,10
        stosb
        ret

;=====================================================================
; Data
;=====================================================================
cmv_on      db  0                       ; 1 = caching
cmv_tested  db  0                       ; the V86 mover has been proved
cmv_allhit  db  0                       ; this read came entirely from here
cmv_last    db  '.'                     ; what the last transfer was, for
                                        ; the port E9h trace
cmv_nora    db  0                       ; no read-ahead: the bounce is busy
cmv_mover   db  0                       ; under a monitor: 1 INT 15h, 2 the
                                        ; XMS entry - cm_selftest decides
cmv_nel     dw  0                       ; elements in use
cmv_kb      dw  0                       ; what the header asked for
cmv_flags   dw  0                       ; RMWC_*
cmv_used    dw  0                       ; entries handed out so far
cmv_hand    dw  0                       ; the clock hand, an entry offset
cmv_dirend  dw  0                       ; one past the last entry in use
cmv_handle  dw  0                       ; the XMS handle it all sits in
cmv_base    dd  0                       ; element 0, flat
cmv_hits    dd  0
cmv_miss    dd  0
cmv_sub     dw  0                       ; the piece: which sector of the
cmv_n       dw  0                       ; element, and how many from there
cmv_ent     dw  0                       ; the entry the piece belongs in
cmv_ecnt    dw  0                       ; sectors of the element the volume has
cmv_elba    dd  0                       ; the element's first sector
cmv_alogn   dw  0                       ; 4A10h calls made
cmv_alog    dw  CM_LOGN * 3 dup(0)    ; BX, DX, BP of the last CM_LOGN
cmv_logname db  'C:\RMCACHE.LOG',0      ; the letter patched in
cmv_xdesc   db  XM_SIZE dup(0)          ; the move, as the XMS entry takes it

cmv_hash    dw  CM_HASHN dup(0)
cmv_dir     db  CM_MAXEL * CE_LEN dup(0)
cmv_bounce  db  CM_EBYTES dup(0)        ; a whole element, read ahead
