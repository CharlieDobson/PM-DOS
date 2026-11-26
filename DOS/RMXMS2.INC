;=====================================================================
; RMXMS2.INC - the extended memory manager RMDOS provides, second part:
;              the entry every client calls, the functions and the
;              block move.  RMXMS.INC is the first part and says what
;              all of it is for; the cut between the two is where
;              RMDOS's stack slots need it to be (RMSTK.INC).
;=====================================================================

;--- rx_owner - EAX inside an allocated block?  CF + ECX = its end ---
rx_owner:
        push    edx
        push    esi
        push    edi
        mov     si,OFFSET rx_handles
        mov     dx,RX_MAXH
ro_loop:
        cmp     word ptr [si+RXH_FLAGS],1
        jne     ro_next
        mov     ecx,[si+RXH_ADDR]
        cmp     eax,ecx
        jb      ro_next
        call    rx_foot
        add     ecx,edi
        cmp     eax,ecx
        jb      ro_hit
ro_next:
        add     si,RXH_LEN
        dec     dx
        jnz     ro_loop
        pop     edi
        pop     esi
        pop     edx
        clc
        ret
ro_hit:
        pop     edi
        pop     esi
        pop     edx
        stc
        ret

;--- rxp_next - ECX = the lowest block start above EAX, or the top ----
rxp_next:
        push    ebx
        push    edx
        push    esi
        mov     ecx,[rx_end]
        mov     si,OFFSET rx_handles
        mov     bx,RX_MAXH
rnx_loop:
        cmp     word ptr [si+RXH_FLAGS],1
        jne     rnx_next
        mov     edx,[si+RXH_ADDR]
        cmp     edx,eax
        jbe     rnx_next
        cmp     edx,ecx
        jae     rnx_next
        mov     ecx,edx
rnx_next:
        add     si,RXH_LEN
        dec     bx
        jnz     rnx_loop
        pop     esi
        pop     edx
        pop     ebx
        ret

;--- rx_scan - EDX = bytes wanted, 0 to only measure -----------------
; CF clear: EAX = where it goes.  CF set: it does not fit.
; EBX = the largest gap in the pool, either way.
rx_scan:
        push    ecx
        push    esi
        push    edi
        xor     ebx,ebx
        mov     eax,[rx_base]
rxs_loop:
        cmp     eax,[rx_end]
        jae     rxs_none
        call    rx_owner
        jc      rxs_skip
        call    rxp_next
        mov     edi,ecx
        sub     edi,eax                 ; the gap in front of us
        cmp     edi,ebx
        jbe     rxs_nomax
        mov     ebx,edi
rxs_nomax:
        or      edx,edx
        jz      rxs_step
        cmp     edi,edx
        jb      rxs_step
        pop     edi
        pop     esi
        pop     ecx
        clc
        ret
rxs_step:
        mov     eax,ecx
        jmp     short rxs_loop
rxs_skip:
        mov     eax,ecx
        add     eax,RX_GRAN - 1
        and     eax,NOT (RX_GRAN - 1)
        jmp     short rxs_loop
rxs_none:
        pop     edi
        pop     esi
        pop     ecx
        stc
        ret

;--- rx_stats - EAX = the largest free block, EDX = all of it, bytes -
rx_stats:
        push    ebx
        push    ecx
        push    esi
        push    edi
        xor     edx,edx
        call    rx_scan
        mov     eax,ebx

        ; REPORTED ROUNDED DOWN TO A PAGE, because that is what can
        ; actually be handed out: a request is rounded UP to one, so a
        ; largest-free-block that is not a whole number of pages is a
        ; number no caller can successfully ask for.
        and     eax,NOT (RX_GRAN - 1)
        mov     edx,[rx_end]
        sub     edx,[rx_base]
        mov     si,OFFSET rx_handles
        mov     cx,RX_MAXH
rt_loop:
        cmp     word ptr [si+RXH_FLAGS],1
        jne     rt_next
        call    rx_foot
        sub     edx,edi
rt_next:
        add     si,RXH_LEN
        loop    rt_loop
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; rx_entry - the far call every XMS client makes.
;
; A program finds this through INT 2Fh AX=4310h and then never uses an
; interrupt again, which is why the entry point has to be sixteen-bit
; code at a real-mode address - here, trivially, because all of RMDOS
; is.  The kernel's own XMS host has to go to some trouble over the
; same requirement; see the note on the HLT trick in XMS.INC.
;
; THE XMS SPECIFICATION SAYS A DRIVER PRESERVES EVERY REGISTER IT DOES
; NOT RETURN A VALUE IN, AND WINDOWS HOLDS IT TO THAT - ALL THIRTY-TWO
; BITS OF EACH.  Until v0.63 this saved the five sixteen-bit registers
; no function returns and let the rest fall where the function left
; them, which is what every caller before WIN386.EXE could live with.
; WIN386's loader cannot.  It reads itself into extended memory 32K at
; a time, keeping the running byte count in ECX ACROSS the 0Bh call -
; it stores the destination offset out of ECX, adds the chunk, calls
; the driver, and then compares ECX with the file size to decide
; whether to go round again.  HIMEM gives ECX back untouched; this
; driver used it as the move length and handed back zero, so the
; comparison never ended the loop and Windows read the end of its own
; file eight hundred thousand times before the time limit.
;
; So the whole set is saved on the way in: ECX, ESI, EDI and EBP on
; the stack, and EAX, EBX and EDX in three dwords, so that rx_ret can
; put back the HALVES a sixteen-bit function did not answer in.  The
; 3.0 functions answer in whole registers and keep what they computed.
;=====================================================================
RX_MAXFN        EQU     12h
RX_REV          EQU     030Ah           ; the driver revision 00h reports:
                                        ; HIMEM 3.10's, not PM-DOS's own, as
                                        ; XMS.INC's xf_00 says why - the WfW
                                        ; 3.11 Setup wants 0260h or more

rx_entry:
        ; A SHORT JUMP AND THREE NOPS FIRST, as HIMEM has them ("for
        ; hookability", himem.asm): five bytes that a program hooking
        ; the driver may overwrite with a far jump of its own, and
        ; which say, by being a short jump, that the chain ends here.
        ; Windows in 386 enhanced mode is such a program - V86MMGR
        ; takes XMS over by planting a breakpoint on the entry - and an
        ; entry that began with PUSH DS was refused with "Cannot install
        ; the 386 enhanced mode XMS driver."
        jmp     short rx_entry2
        nop
        nop
        nop
rx_entry2:
        push    ds
        push    es
        push    esi
        push    edi
        push    ebp
        push    ecx
        mov     byte ptr cs:[rx_svfn],ah
        mov     dword ptr cs:[rx_sveax],eax
        mov     dword ptr cs:[rx_svebx],ebx
        mov     dword ptr cs:[rx_svedx],edx

        cmp     ah,0Bh
        jne     rx_nodesc

        ; The move descriptor, copied in WHILE DS:SI STILL MEANS WHAT
        ; THE CALLER MEANT.  Everything below this line reaches this
        ; file's data through DS, and a routine that keeps the caller's
        ; DS and then calls anything is the bug rd_path had.
        push    ax
        push    cx
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rx_desc
        mov     cx,XM_SIZE / 2
        cld
        rep     movsw
        pop     cx
        pop     ax
rx_nodesc:
        mov     si,cs
        mov     ds,si
        mov     es,si
IFDEF DBG_LPT
        call    rdbg_xin                ; the question, held until the answer
ENDIF
IFDEF RMDOS_E9
        call    e9_xin                  ; the same question on port E9h
ENDIF
        cmp     ah,RX_MAXFN
        ja      rx_high
        movzx   si,ah
        shl     si,1
        jmp     word ptr [rx_tab + si]

;--- the XMS 3.0 additions, which are the same questions in EAX -----
rx_high:
        cmp     ah,88h
        je      rxf88
        cmp     ah,89h
        je      rxf89
        cmp     ah,8Eh
        je      rxf8E
        cmp     ah,8Fh
        je      rxf8F
        mov     bl,RXE_NOTIMPL
        jmp     short rxr_err

rxr_ok:
        mov     ax,1
        jmp     short rx_ret
rxr_err:                                 ; BL = why
        xor     ax,ax
rx_ret:
IFDEF DBG_LPT
        call    rdbg_xout               ; ...and both, recorded together
ENDIF
IFDEF RMDOS_E9
        call    e9_xout
ENDIF
        ; What goes back: see the head of rx_entry.  88h answers in
        ; EAX, ECX and EDX whole; 8Eh in EDX whole; everything else in
        ; sixteen-bit halves, so the top halves are the caller's and go
        ; back to it, and ECX goes back to every function but 88h.
        cmp     byte ptr cs:[rx_svfn],88h
        je      rxr_whole
        cmp     byte ptr cs:[rx_svfn],8Eh
        je      rxr_edx
        and     dword ptr cs:[rx_svedx],0FFFF0000h
        and     edx,0FFFFh
        or      edx,dword ptr cs:[rx_svedx]
rxr_edx:
        and     dword ptr cs:[rx_sveax],0FFFF0000h
        and     eax,0FFFFh
        or      eax,dword ptr cs:[rx_sveax]
        and     dword ptr cs:[rx_svebx],0FFFF0000h
        and     ebx,0FFFFh
        or      ebx,dword ptr cs:[rx_svebx]
        pop     ecx
        jmp     short rxr_out
rxr_whole:
        add     sp,4                    ; the saved ECX, not wanted
rxr_out:
        pop     ebp
        pop     edi
        pop     esi
        pop     es
        pop     ds
        retf

rx_svfn     db  0                       ; the function, for rx_ret
rx_sveax    dd  0                       ; the caller's EAX, EBX and EDX,
rx_svebx    dd  0                       ; whose top halves no sixteen-bit
rx_svedx    dd  0                       ; function answers in

;=====================================================================
; 00h - what this driver is.
;
; AX = 0300h is the SPECIFICATION it implements, BX its own revision,
; and DX whether an HMA exists - which depends on where this image
; parked, and is worked out rather than assumed.  See rxf01.
;=====================================================================
rxf00:
        mov     bx,RX_REV
        mov     dx,1                    ; an HMA exists either way: low it is
        mov     ax,0300h                ; the guest's, high it is this driver's
        jmp     rx_ret                  ; own - see rxf01

;=====================================================================
; 01h / 02h - the HMA.
;
; IT DEPENDS ON WHERE THIS IMAGE IS, and until v0.53 it did not: the
; answer was a flat "no HMA exists", written when RMDOS always parked
; into the HMA and was therefore standing in the only 64K it could
; have handed out.  RMDOS parks LOW by default now - the Windows
; kernel scans for the SFT only as far as 80000h and cannot find a DOS
; above it - so on the ordinary path the HMA is empty and this was
; refusing memory that nobody was using.
;
; And it is refusing it to the one program that most wants it.  The
; XMS log of a standard-mode start reads:
;
;       AH=01 req HMA  DX=FFFF  ->  AX=0000  FAILED BL=90
;
; DX=FFFFh is the specification's "I am an application, not a TSR, so
; the HMAMIN threshold does not apply to me" - which is DOSX asking
; for the whole of it, exactly as it would of HIMEM.
;
; The 64K IS FREE, and by construction rather than by luck: PMDOS.INC
; leaves 100000h-10FFEFh empty on purpose and loads the kernel at
; 110000h instead, precisely so that a guest's FFFF:xxxx means what it
; says.  So when this image is low, the HMA is the guest's to ask for,
; and when it is high the old answer is still the right one - it is
; this driver's own code up there.
;
; A20 is the caller's business, as it always is: the HMA does not
; exist as an address until the gate is open, and 03h/05h are how a
; program says so.
;=====================================================================
rxf01:
        test    byte ptr [h_flags],RMHF_HIGH
        jnz     f01_nohma               ; we are standing in it
        cmp     byte ptr [rx_hmaown],0
        jne     f01_inuse
        cmp     dx,0FFFFh
        je      f01_grant               ; an application takes all of it
        cmp     dx,RX_HMAMIN
        jb      f01_toosmall
f01_grant:
        mov     byte ptr [rx_hmaown],1
        jmp     rxr_ok
f01_nohma:
        ; IN USE, NOT ABSENT.  The HMA exists and this driver is standing
        ; in it, which is the answer a DOS loaded high gives and the one
        ; Windows understands: V86MMGR takes a refused HMA to mean that
        ; something lives there and keeps A20 on for every machine.
        mov     bl,RXE_HMAINUSE
        jmp     rxr_err
f01_inuse:
        mov     bl,RXE_HMAINUSE
        jmp     rxr_err
f01_toosmall:
        mov     bl,RXE_HMAMIN
        jmp     rxr_err

rxf02:
        test    byte ptr [h_flags],RMHF_HIGH
        jnz     f01_nohma
        cmp     byte ptr [rx_hmaown],0
        je      f02_notours
        mov     byte ptr [rx_hmaown],0
        jmp     rxr_ok
f02_notours:
        mov     bl,RXE_HMANOTUSED
        jmp     rxr_err

;=====================================================================
; 03h-07h - the A20 gate.
;
; GLOBAL and LOCAL are two different questions and the specification
; means both.  A global enable is a program saying "leave it open"; a
; local one is a program saying "I need it for the next few
; instructions" and pairs with a local disable.  So local enables are
; COUNTED and a global disable will not close a gate somebody is
; standing in - which is the one place these five functions can do
; real damage, because the program holding it open is reading memory
; that stops existing the moment the gate shuts.
;=====================================================================
rxf03:
        call    rx_a20raise
        mov     al,1
        jmp     short rx_a20done
rxf04:
        cmp     word ptr [rx_a20cnt],0
        jne     rxf04_held
        call    rx_a20lower
        xor     al,al
        jmp     short rx_a20done
rxf04_held:
        mov     bl,RXE_A20STILL
        jmp     rxr_err

rxf05:
        inc     word ptr [rx_a20cnt]
        call    rx_a20raise
        mov     al,1
        jmp     short rx_a20done
rxf06:
        cmp     word ptr [rx_a20cnt],0
        je      rxf06_none
        dec     word ptr [rx_a20cnt]
        mov     al,1
        jnz     rx_a20done              ; somebody else still holds it
        call    rx_a20lower
        xor     al,al
        jmp     short rx_a20done
rxf06_none:
        mov     bl,RXE_A20
        jmp     rxr_err

; AL = THE STATE WE ASKED FOR, and every one of these ends by LOOKING
; rather than by assuming the hardware did what it was told.  A
; machine whose gate this driver cannot move is a machine where 03h
; has to FAIL - not one where it reports success and the caller then
; reads the wrong megabyte.
rx_a20done:
        mov     [rx_a20want],al
        ; ...unless the answer was "shut it" and we are living up
        ; there, in which case the gate is deliberately still open and
        ; saying so would fail a call the caller can do nothing about.
        ; A wired-open gate is a configuration, not an error.
        or      al,al
        jnz     rxa_check
        test    byte ptr [h_flags],RMHF_HIGH
        jz      rxa_check
        jmp     rxr_ok
rxa_check:
        call    rx_a20test
        cmp     al,[rx_a20want]
        jne     rxa_bad
        jmp     rxr_ok
rxa_bad:
        mov     bl,RXE_A20
        jmp     rxr_err

;--- 07h - is it open?  AX = 1 or 0 and BL = 0 either way -----------
rxf07:
        call    rx_a20test
        xor     bl,bl
        jmp     rx_ret

;=====================================================================
; rx_alloc - EDX = kilobytes wanted.
;            CF clear: BX = the handle.  CF set: BL = why not.
;
; DESTROYS EBX, which is the answer.  Said here because the last time
; a routine in this tree answered in a register, a later edit saved
; and restored it and every caller got its own value back.
;=====================================================================
rx_alloc:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     ecx,edx                 ; the size, as asked for
        cmp     ecx,400000h             ; four gigabytes of kilobytes
        jae     ra_nomem
        call    rx_newh                 ; -> SI, DI
        jc      ra_nohandle
        mov     edx,ecx
        shl     edx,10
        add     edx,RX_GRAN - 1
        and     edx,NOT (RX_GRAN - 1)
        jz      ra_zero                 ; a handle for no memory is legal
        call    rx_scan
        jc      ra_nomem
        jmp     short ra_got
ra_zero:
        mov     eax,[rx_base]
ra_got:
        mov     word ptr [si+RXH_FLAGS],1
        mov     [si+RXH_KB],ecx
        mov     [si+RXH_ADDR],eax
        mov     word ptr [si+RXH_LOCK],0
        mov     bx,di
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
ra_nohandle:
        mov     bl,RXE_NOHANDLE
        jmp     short ra_bad
ra_nomem:
        mov     bl,RXE_NOMEM
ra_bad:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; 08h / 88h - how much is there?
;
; 08h answers in kilobytes in sixteen bits, which saturates at 64MB
; minus a kilobyte and has done since 1988.  88h is the same question
; asked in EAX, and is the reason this driver reports version 3.00
; rather than 2.00.
;=====================================================================
rxf08:
        call    rx_stats                ; EAX largest, EDX total, in bytes
        shr     eax,10
        shr     edx,10
        cmp     eax,0FFFFh
        jbe     f08_cap1
        mov     eax,0FFFFh
f08_cap1:
        cmp     edx,0FFFFh
        jbe     f08_cap2
        mov     edx,0FFFFh
f08_cap2:
        xor     bl,bl
        or      ax,ax
        jnz     f08_out
        mov     bl,RXE_NOMEM
f08_out:
        jmp     rx_ret

rxf88:
        call    rx_stats
        shr     eax,10
        shr     edx,10
        mov     ecx,[rx_end]
        or      ecx,ecx
        jz      f88_none
        dec     ecx                     ; the highest address, not the top
f88_none:
        xor     bl,bl
        or      eax,eax
        jnz     f88_out
        mov     bl,RXE_NOMEM
f88_out:
        jmp     rx_ret

;--- 09h / 89h - allocate.  DX or EDX = KB, DX = the handle back ----
rxf09:
        movzx   edx,dx
        jmp     short rxf09_common
rxf89:
                                        ; 89h: EDX is already the size
rxf09_common:
        call    rx_alloc
        jc      f09_bad
        mov     dx,bx
        xor     bl,bl
        jmp     rxr_ok
f09_bad:
        jmp     rxr_err

;--- 0Ah - free.  A locked block is somebody's DMA target -----------
rxf0A:
        mov     bx,dx
        call    rx_find
        jc      f0A_bad
        cmp     word ptr [si+RXH_LOCK],0
        jne     f0A_locked
        mov     word ptr [si+RXH_FLAGS],0
        jmp     rxr_ok
f0A_bad:
        mov     bl,RXE_BADHANDLE
        jmp     rxr_err
f0A_locked:
        mov     bl,RXE_LOCKED
        jmp     rxr_err

;=====================================================================
; 0Ch / 0Dh - lock and unlock.
;
; A LOCK IS A PROMISE NOT TO MOVE THE BLOCK, and it is answered with
; the block's PHYSICAL address, because the caller is about to give
; that number to something that does not go through the processor at
; all - a disk controller, or a page table.  This driver never moves a
; block behind its owner's back, so the promise costs nothing to keep
; and the count exists to stop 0Ah and 0Fh from breaking it.
;
; The number is the truth here only because PM-DOS's page map is the
; identity below its own ceiling, so a linear address IS a physical
; one.  A manager that paged would have to pin the pages first.
;=====================================================================
rxf0C:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        cmp     word ptr [si+RXH_LOCK],0FFh
        jae     f0C_toomany
        inc     word ptr [si+RXH_LOCK]
        mov     eax,[si+RXH_ADDR]
        mov     bx,ax
        shr     eax,16
        mov     dx,ax
        jmp     rxr_ok
f0C_bad:
        mov     bl,RXE_BADHANDLE
        jmp     rxr_err
f0C_toomany:
        mov     bl,RXE_LOCKCOUNT
        jmp     rxr_err

rxf0D:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        cmp     word ptr [si+RXH_LOCK],0
        je      f0D_notlocked
        dec     word ptr [si+RXH_LOCK]
        jmp     rxr_ok
f0D_notlocked:
        mov     bl,RXE_NOTLOCKED
        jmp     rxr_err

;--- 0Eh / 8Eh - what is this handle? -------------------------------
rxf0E:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        mov     eax,[si+RXH_KB]
        cmp     eax,0FFFFh
        jbe     f0E_fits
        mov     eax,0FFFFh
f0E_fits:
        mov     dx,ax
        call    rx_freeh                ; -> CX
        mov     ax,word ptr [si+RXH_LOCK]
        mov     bh,al
        mov     bl,cl
        jmp     rxr_ok

rxf8E:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        mov     edx,[si+RXH_KB]
        call    rx_freeh
        mov     ax,word ptr [si+RXH_LOCK]
        mov     bh,al
        mov     bl,cl
        jmp     rxr_ok

;--- 10h/11h/12h - upper memory, which came from EMM386 and not -----
;    from HIMEM.  See the head of this file.
rxf10:
        xor     dx,dx                   ; and none available, either
        mov     bl,RXE_NOUMB
        jmp     rxr_err
rxf11:
        mov     bl,RXE_BADUMB
        jmp     rxr_err
rxf12:
        mov     bl,RXE_NOUMB
        jmp     rxr_err

;=====================================================================
; 0Fh / 8Fh - reallocate.
;
; Shrinking is a change to one field.  Growing takes the block OUT of
; the map before looking for room, so that the scan can offer the
; space the block is already standing in - which is where a block that
; grows almost always ends up, and a scan that could not see it would
; move a megabyte for nothing.  If it does have to move, the contents
; go with it: a realloc that loses the data is not a realloc.
;=====================================================================
rxf0F:
        movzx   ebx,bx
        jmp     short rx_realloc
rxf8F:
        jmp     short rx_realloc        ; EBX is already the whole size

rx_realloc:
        mov     [rx_newkb],ebx
        mov     bx,dx
        call    rx_find
        jc      f0F_badh
        mov     [rx_rec],si
        cmp     word ptr [si+RXH_LOCK],0
        jne     f0F_locked

        call    rx_foot                 ; -> EDI, what it occupies now
        mov     [rx_oldfoot],edi
        mov     edx,[rx_newkb]
        cmp     edx,400000h
        jae     f0F_nomem
        shl     edx,10
        add     edx,RX_GRAN - 1
        and     edx,NOT (RX_GRAN - 1)
        cmp     edx,edi
        jbe     f0F_fits

        mov     word ptr [si+RXH_FLAGS],0
        call    rx_scan
        mov     si,[rx_rec]
        mov     word ptr [si+RXH_FLAGS],1
        jc      f0F_nomem
        cmp     eax,[si+RXH_ADDR]
        je      f0F_fits

        mov     [rx_newaddr],eax
        mov     ecx,[si+RXH_ADDR]
        mov     esi,ecx
        mov     edi,[rx_newaddr]
        mov     ecx,[rx_oldfoot]
        call    rx_domove
        mov     si,[rx_rec]
        mov     eax,[rx_newaddr]
        mov     [si+RXH_ADDR],eax
f0F_fits:
        mov     si,[rx_rec]
        mov     eax,[rx_newkb]
        mov     [si+RXH_KB],eax
        jmp     rxr_ok
f0F_badh:
        mov     bl,RXE_BADHANDLE
        jmp     rxr_err
f0F_locked:
        mov     bl,RXE_LOCKED
        jmp     rxr_err
f0F_nomem:
        mov     bl,RXE_NOMEM
        jmp     rxr_err

;=====================================================================
; 0Bh - move a block, which is the function this whole file is built
; around and the only one a real HIMEM needs protected mode for.
;
; Either end may be a handle with an offset into it, or handle zero
; with a real-mode SEGMENT:OFFSET in the same dword - which is how a
; program moves data between conventional and extended memory, and
; why this driver has to understand both kinds of address.
;=====================================================================
rxf0B:
        mov     ecx,dword ptr [rx_desc + XM_LEN]
        test    ecx,1
        jnz     f0B_badlen
        or      ecx,ecx
        jz      xmv_none                ; nothing to do, done successfully

        mov     bx,word ptr [rx_desc + XM_SRCH]
        mov     edx,dword ptr [rx_desc + XM_SRCO]
        call    rx_resolve
        jc      f0B_srcbad
        mov     [rx_msrc],eax

        mov     bx,word ptr [rx_desc + XM_DSTH]
        mov     edx,dword ptr [rx_desc + XM_DSTO]
        call    rx_resolve
        jc      f0B_dstbad

        mov     edi,eax
        mov     esi,[rx_msrc]
        mov     ecx,dword ptr [rx_desc + XM_LEN]
        call    rx_domove
        jc      f0B_failed
xmv_none:
        jmp     rxr_ok
f0B_badlen:
        mov     bl,RXE_BADLEN
        jmp     rxr_err
f0B_srcbad:
        mov     bl,RXE_BADSRCH
        or      al,al
        jz      f0B_srcout
        mov     bl,RXE_BADSRCO
f0B_srcout:
        jmp     rxr_err
f0B_dstbad:
        mov     bl,RXE_BADDSTH
        or      al,al
        jz      f0B_dstout
        mov     bl,RXE_BADDSTO
f0B_dstout:
        jmp     rxr_err
f0B_failed:
        mov     bl,RXE_PARITY
        jmp     rxr_err

;---------------------------------------------------------------------
; rx_resolve - BX = handle, EDX = offset, ECX = length.
;              CF clear: EAX = the flat address.
;              CF set:   AL = 0 the handle is wrong, 1 the offset is.
;---------------------------------------------------------------------
rx_resolve:
        push    ecx
        push    edi
        push    esi
        or      bx,bx
        jnz     rr_handle

        ; Handle zero: the dword is a real-mode pointer, and this is
        ; the one place in the driver where a segment still means
        ; sixteen times something.
        mov     eax,edx
        shr     eax,16
        shl     eax,4
        movzx   edx,dx
        add     eax,edx
        jmp     short rr_ok
rr_handle:
        call    rx_find
        jc      rr_badh
        mov     eax,[si+RXH_KB]
        shl     eax,10
        mov     edi,edx
        add     edi,ecx                 ; the far end of what was asked for
        jc      rr_bado                 ; which must not have wrapped
        cmp     edi,eax
        ja      rr_bado
        mov     eax,[si+RXH_ADDR]
        add     eax,edx
rr_ok:
        pop     esi
        pop     edi
        pop     ecx
        clc
        ret
rr_badh:
        mov     al,0
        jmp     short rr_bad
rr_bado:
        mov     al,1
rr_bad:
        pop     esi
        pop     edi
        pop     ecx
        stc
        ret

;=====================================================================
; rx_domove - ESI, EDI, ECX.  CF set if it could not be done.
;
; The gate has to be open for the length of it, whatever the caller's
; A20 state was, because at least one end of the move is above the
; megabyte by definition - and it has to go back to what it was
; afterwards, because a program that had A20 shut and finds it open
; is a program reading somebody else's memory.
;=====================================================================
rx_domove:
        or      ecx,ecx
        jz      dm_nothing
        pushf
        cli
        call    rx_a20test
        push    ax                      ; how we found the gate
        call    rx_a20raise

IFDEF RMDOS_I15MOVE
        call    rx_move15               ; forced, so that the path a
ELSE                                    ; monitor would take gets run
        smsw    ax
        test    al,1                    ; PE set: protected mode is not
        jnz     dm_notours              ; ours to leave
        call    rx_moveflat
        jmp     short dm_gate
dm_notours:
        call    rx_move15
ENDIF
dm_gate:
        mov     byte ptr [rx_mverr],0
        jnc     dm_kept
        mov     byte ptr [rx_mverr],1
dm_kept:
        pop     ax
        or      al,al
        jnz     dm_wasopen
        call    rx_a20lower
dm_wasopen:
        popf                            ; and CF is the CALLER'S again,
        cmp     byte ptr [rx_mverr],0   ; which is why the answer waited
        je      dm_nothing              ; in memory to be put back here
        stc
        ret
dm_nothing:
        clc
        ret

;=====================================================================
; rx_moveflat - the real thing: leave real mode, load two descriptors
; that cover the whole address space, come back, and copy.
;
; THE SEGMENT REGISTERS KEEP WHAT THEY WERE GIVEN.  A 386 loads a
; descriptor into a hidden cache when a segment register is written
; and uses the cache, not the descriptor, until it is written again -
; so a DS loaded in protected mode with a four-gigabyte limit still
; has one after the processor goes back to real mode.  The copy below
; is ordinary real-mode code addressing the whole machine, and this is
; the same trick every HIMEM has used since 1988.
;
; The rules it lives under: interrupts are already off, nothing may
; reload DS or ES until the copy is finished, and nothing in between
; may touch this file's own data - because DS no longer reaches it.
;=====================================================================
rx_moveflat:
        push    ds
        push    es
        push    ebp

        ; Which way to copy, decided here because deciding it later
        ; would mean reading a variable we can no longer address.
        xor     ebp,ebp                 ; 0 = forwards
        cmp     edi,esi
        jbe     mf_dir
        mov     eax,esi
        add     eax,ecx
        cmp     edi,eax
        jae     mf_dir
        mov     ebp,1                   ; they overlap and the destination
mf_dir:                                 ; is the higher: copy downwards

        db      66h                     ; so that all thirty-two bits of
        lgdt    fword ptr cs:[rx_gdtr]  ; the base are loaded
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        jmp     short mf_pe
mf_pe:
        mov     ax,8                    ; the flat data descriptor
        mov     ds,ax
        mov     es,ax
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        jmp     short mf_rm
mf_rm:
        or      ebp,ebp
        jnz     mf_backward

        mov     edx,ecx
        and     edx,3
        shr     ecx,2
        or      ecx,ecx
        jz      mf_tail
mf_dwords:
        mov     eax,ds:[esi]
        mov     es:[edi],eax
        add     esi,4
        add     edi,4
        dec     ecx
        jnz     mf_dwords
mf_tail:
        mov     ecx,edx
        or      ecx,ecx
        jz      mf_end
mf_bytes:
        mov     al,ds:[esi]
        mov     es:[edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     mf_bytes
        jmp     short mf_end

mf_backward:
        add     esi,ecx
        add     edi,ecx
mf_bloop:
        dec     esi
        dec     edi
        mov     al,ds:[esi]
        mov     es:[edi],al
        dec     ecx
        jnz     mf_bloop
mf_end:
        ; and these two POPs are what puts the sixty-four kilobyte
        ; limits back - loading a segment register in real mode
        ; rewrites the cache it was carrying
        pop     ebp
        pop     es
        pop     ds
        clc
        ret

IFDEF XMSPOOLSTAMP
;=====================================================================
; rx_pooltest - is the memory at the address this driver reports the
;               same memory it actually handed over?
;
; THE QUESTION THE FAULT LEFT BEHIND.  Windows 3.1 standard mode dies
; on an IRET inside DOSX with the client's own stack selector loaded,
; and the stack behind that selector reads as zeros - before DOSX has
; written anything.  The selector's base is memory DOSX got from this
; driver through DPMI 0501h, so either the kernel's copy into it never
; landed, or THE ADDRESS THIS DRIVER REPORTS IS NOT THE MEMORY IT GAVE
; AWAY.  The second is ours to get wrong, so it is the one to rule out
; first, and it can be ruled out here without Windows in the room.
;
; The test is the only one that distinguishes the two: every page in
; the pool is stamped with ITS OWN LINEAR ADDRESS and then read back.
; A pool that answers correctly everywhere is a pool of distinct,
; present, correctly-addressed memory.  A pool where page X reads back
; the stamp of page Y is aliased, and the difference X-Y names the
; address line that is not getting through - A20 at 100000h, or a chip
; select the emulated 440BX never decodes, or a bug of ours.  A pool
; that reads back nothing at all is not there.
;
; It runs where the answer counts: after the park, in real mode, with
; PM-DOS's page tables gone, which is exactly the machine Windows will
; see.  A20 is forced open for the duration and put back afterwards,
; because half the pool has bit 20 set and testing it through a shut
; gate would only rediscover the gate.
;
; Stamping destroys the pool's contents, which is why this is DBG_LPT
; only and why it runs from rx_init - before any handle exists, so
; there is nothing in there to destroy.
;=====================================================================
rx_pooltest:
        pushad
        pushf
        cli
        mov     dword ptr [rx_ptbad],0FFFFFFFFh
        mov     dword ptr [rx_ptval],0
        mov     byte ptr [rx_ptflg],0

        call    rx_a20test
        or      al,al
        jz      pt_shut
        or      byte ptr [rx_ptflg],1   ; how the gate was found, because a
pt_shut:                                ; gate that was shut explains a lot
        call    rx_a20raise

        ; LEAVING REAL MODE HAS TO BE OURS TO DO.  The same test
        ; rx_domove makes, for the same reason: under a monitor CR0
        ; belongs to somebody else, and there is no INT 15h service
        ; that would do this one.
        smsw    ax
        test    al,1
        jz      pt_run
        or      byte ptr [rx_ptflg],2
        jmp     short pt_gate
pt_run:
        mov     esi,[rx_base]
        mov     ebp,[rx_end]
        call    rx_ptflat
        mov     [rx_ptbad],edi
        mov     [rx_ptval],edx
        cmp     edi,0FFFFFFFFh
        jne     pt_gate
        mov     [rx_ptval],ecx          ; nothing wrong: say how much was
pt_gate:                                ; checked, so that a silent pass
        test    byte ptr [rx_ptflg],1   ; is still a measurement
        jnz     pt_wasopen
        call    rx_a20lower
pt_wasopen:
        popf
        popad
        ret

;---------------------------------------------------------------------
; rx_ptflat - the two passes, in unreal mode.  ESI = base, EBP = end.
;
;   EDI = the first address that lied, or FFFFFFFFh
;   EDX = what it said instead
;   ECX = pages checked
;
; Under the same rules as rx_moveflat: interrupts already off, and
; nothing between the two segment loads may touch this file's data,
; because DS no longer reaches it.  Both ends of the pool arrive in
; registers for that reason.
;---------------------------------------------------------------------
rx_ptflat:
        push    ds
        push    es
        push    ebx
        push    esi
        push    ebp

        db      66h
        lgdt    fword ptr cs:[rx_gdtr]
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        jmp     short pf_pe
pf_pe:
        mov     ax,8                    ; the flat data descriptor
        mov     ds,ax
        mov     es,ax
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        jmp     short pf_rm
pf_rm:
        ; EVERY DWORD, not one per page.  A stamp every 4096 bytes
        ; answers "is this page distinct memory", which was the
        ; aliasing question and is now settled; it cannot answer "did
        ; anything write HERE", because a stack 0030h into a page is
        ; nowhere near the one word being watched.  Thirty-two million
        ; stamps take a few seconds of emulated time and turn any read
        ; of extended memory, at any offset, into an oracle: the dword
        ; either says its own address or says somebody wrote it.
        mov     ebx,esi                 ; pass one
        xor     ecx,ecx
pf_stamp:
        cmp     ebx,ebp
        jae     pf_verify
        mov     ds:[ebx],ebx
        add     ebx,4
        inc     ecx
        jmp     short pf_stamp
pf_verify:
        mov     ebx,esi                 ; pass two
pf_check:
        cmp     ebx,ebp
        jae     pf_ok
        mov     eax,ds:[ebx]
        cmp     eax,ebx
        jne     pf_bad
        add     ebx,4
        jmp     short pf_check
pf_bad:
        mov     edi,ebx
        mov     edx,eax
        jmp     short pfs_out
pf_ok:
        mov     edi,0FFFFFFFFh
        xor     edx,edx
pfs_out:
        pop     ebp                     ; and these put the real-mode
        pop     esi                     ; limits back
        pop     ebx
        pop     es
        pop     ds
        ret

;=====================================================================
; rx_poolscan - and afterwards, WHICH PAGES LOST THEIR STAMP.
;
; The stamps rx_pooltest leaves are only half an instrument.  The other
; half is asking, once the excursion is over, which of them are gone -
; because a page that no longer holds its own address is a page
; something WROTE TO, and the set of those pages is a map of every byte
; of extended memory Windows actually used.
;
; That map is the question the standard-mode fault reduces to.  DOSX
; hands KRNL386 a block out of this pool and the kernel repoints its
; own stack selector at it; the memory behind that selector reads back
; its stamp, untouched, so the kernel's copy went SOMEWHERE ELSE.  A
; count of nothing says the DPMI memory path never wrote at all; a
; count confined to the low block says the high one cannot be reached.
;
; Called from rd_goback under RMDOS_FREEZE, when there is nothing left
; to disturb.
;=====================================================================
rx_poolscan:
        pushad
        pushf
        cli
        call    rx_a20test
        push    ax
        call    rx_a20raise
        smsw    ax
        test    al,1
        jnz     ps_gate
        mov     esi,[rx_base]
        mov     ebp,[rx_end]
        call    rx_psflat
        mov     [rx_psn],ecx
        mov     [rx_psfirst],edi
        mov     [rx_pslast],edx

        ; AND THE ADDRESSES THEMSELVES, which is the map.  One scan per
        ; entry, restarting a page past the last one found: twenty-four
        ; passes over 32000 pages is nothing next to a boot, and it
        ; needs no register inside the unreal loop to hold a table
        ; pointer - there are none spare in there, and DS is flat, so
        ; the loop could not reach this data anyway.
        mov     bx,OFFSET rx_pslist
        mov     cx,RX_PSMAX
        mov     esi,[rx_base]
psl_loop:
        push    cx
        push    bx
        mov     ebp,[rx_end]
        call    rx_psflat
        pop     bx
        pop     cx
        cmp     edi,0FFFFFFFFh
        je      ps_gate
        mov     [bx],edi
        add     bx,4
        mov     esi,edi
        add     esi,1000h
        loop    psl_loop
ps_gate:
        pop     ax
        or      al,al
        jnz     ps_wasopen
        call    rx_a20lower
ps_wasopen:
        popf
        popad
        ret

;--- the scan itself, unreal, under rx_ptflat's rules ----------------
; ESI = base, EBP = end.  ECX = pages written, EDI = the first of them,
; EDX = the last.
rx_psflat:
        push    ds
        push    es
        push    ebp
        push    esi

        db      66h
        lgdt    fword ptr cs:[rx_gdtr]
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        jmp     short ps_pe
ps_pe:
        mov     ax,8
        mov     ds,ax
        mov     es,ax
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        jmp     short ps_rm
ps_rm:
        mov     ebx,esi
        xor     ecx,ecx
        mov     edi,0FFFFFFFFh
        xor     edx,edx
ps_loop:
        cmp     ebx,ebp
        jae     ps_out
        mov     esi,ebx                 ; ESI is free once EBX has the
ps_page:                                ; base: every dword of this page
        mov     eax,ds:[esi]
        cmp     eax,esi
        jne     ps_dirty
        add     esi,4
        test    si,0FFFh                ; a page ends when the low twelve
        jnz     ps_page                 ; bits come back to zero
        jmp     short ps_next
ps_dirty:
        inc     ecx
        mov     edx,ebx                 ; the last page seen, always
        cmp     edi,0FFFFFFFFh
        jne     ps_next
        mov     edi,ebx                 ; and the first, once
ps_next:
        add     ebx,1000h
        jmp     short ps_loop
ps_out:
        pop     esi
        pop     ebp
        pop     es
        pop     ds
        ret

;=====================================================================
; rx_psshow - the three numbers onto the bottom line of the screen.
;
; Written straight into the text page rather than through anything,
; because by the time this runs the excursion has ended and there is no
; DOS underneath it - and because the line above is a fault report this
; must not scroll away.
;=====================================================================
rx_psshow:
        pushad
        push    es
        mov     ax,0B800h
        mov     es,ax
        mov     di,24*160
        mov     eax,[rx_psn]
        call    rx_pshex
        add     di,4
        mov     eax,[rx_psfirst]
        call    rx_pshex
        add     di,4
        mov     eax,[rx_pslast]
        call    rx_pshex

        ; the map itself, eight to a row, on the three rows above
        mov     si,OFFSET rx_pslist
        mov     cx,RX_PSMAX
        mov     di,19*160
        xor     bp,bp
pss_loop:
        mov     eax,[si]
        or      eax,eax
        jz      pss_done
        call    rx_pshex
        add     di,2
        add     si,4
        inc     bp
        cmp     bp,8
        jb      pss_next
        xor     bp,bp
        add     di,160
        sub     di,8*18                 ; back to column 0 of the next row
pss_next:
        loop    pss_loop
pss_done:
        pop     es
        popad
        ret

;--- EAX in hex at ES:DI, bright white, DI left after it ------------
rx_pshex:
        push    ecx
        push    bx
        mov     cx,8
psh_l:
        rol     eax,4
        mov     bl,al
        and     bl,0Fh
        add     bl,'0'
        cmp     bl,'9'
        jbe     psh_dig
        add     bl,7
psh_dig:
        mov     byte ptr es:[di],bl
        mov     byte ptr es:[di+1],1Fh
        add     di,2
        loop    psh_l
        pop     bx
        pop     ecx
        ret
ENDIF

;=====================================================================
; rx_move15 - the same move, asked of the machine instead of done.
;
; INT 15h AH=87h is the BIOS's own block move: it makes the switch
; itself, from a descriptor table the caller fills in.  It is here for
; the case where LEAVING REAL MODE IS NOT OURS TO DO - once Windows is
; running this driver's code is inside a virtual 8086 machine and CR0
; belongs to somebody else.  SMSW answers that question without being
; privileged: PE is clear in real mode and set under a monitor.
;
; Taken rather than refusing, because a monitor that puts a DOS box in
; V86 mode virtualises 87h - it is how everything moved extended
; memory before XMS was written, so everything that virtualises DOS
; handles it.
;=====================================================================
rx_move15:
        push    ds
        push    es
        push    ebp
mv15_loop:
        or      ecx,ecx
        jz      mv15_done
        mov     ax,cs                   ; each pass, because the BIOS is
        mov     ds,ax                   ; entitled to both of these
        mov     es,ax
        mov     eax,ecx
        cmp     eax,10000h              ; 8000h words is all 87h takes
        jbe     mv15_size
        mov     eax,10000h
mv15_size:
        mov     ebp,eax

        mov     bx,OFFSET rx_gdt87 + 10h
        call    mv15_desc
        push    esi
        mov     esi,edi
        mov     bx,OFFSET rx_gdt87 + 18h
        call    mv15_desc
        pop     esi

        push    ecx
        push    esi
        mov     ecx,ebp
        shr     ecx,1
        mov     si,OFFSET rx_gdt87
        mov     ah,87h
        int     15h
        pop     esi
        pop     ecx
        jc      mv15_fail
        sub     ecx,ebp
        add     esi,ebp
        add     edi,ebp
        jmp     short mv15_loop
mv15_done:
        pop     ebp
        pop     es
        pop     ds
        clc
        ret
mv15_fail:
        pop     ebp
        pop     es
        pop     ds
        stc
        ret

; BX -> a descriptor, ESI = its base, EBP = its length
mv15_desc:
        push    eax
        mov     eax,ebp
        dec     eax
        mov     [bx],ax                 ; limit
        mov     eax,esi
        mov     [bx+2],ax               ; base 15-0
        shr     eax,16
        mov     [bx+4],al               ; base 23-16
        mov     byte ptr [bx+5],93h     ; present, writable data
        mov     word ptr [bx+6],0
        mov     [bx+7],ah               ; base 31-24, which the first BIOS
        pop     eax                     ; to define this ignored
        ret

;=====================================================================
; Data
;=====================================================================
rx_base     dd  0                       ; the pool PM-DOS handed over
rx_end      dd  0
IFDEF XMSPOOLSTAMP
rx_ptbad    dd  0FFFFFFFFh              ; rx_pooltest: the first address that
rx_ptval    dd  0                       ; lied, what it said, and how it was
rx_ptflg    db  0                       ; found - see rdbg_xpt
rx_psn      dd  0                       ; rx_poolscan: pages that lost
rx_psfirst  dd  0                       ; their stamp, and the first and
rx_pslast   dd  0                       ; last of them
rx_pslist   dd  RX_PSMAX dup (0)        ; and where the first of them are
ENDIF
rx_hmaown   db  0                       ; the HMA, when this image is low
rx_a20cnt   dw  0                       ; local enables outstanding
rx_a20meth  db  0                       ; 0 none, 1 the 8042, 2 port 92h
rx_a20want  db  0
rx_mverr    db  0
rx_oldi15   dd  0
rx_msrc     dd  0
rx_rec      dw  0
rx_newkb    dd  0
rx_newaddr  dd  0
rx_oldfoot  dd  0
rx_desc     db  XM_SIZE dup(0)

; The descriptor table the block move switches through: the null
; descriptor the processor insists on, and one that covers the whole
; address space.  The three after them are the way into the parked
; kernel's (RMBRG.INC): flat 32-bit code, 16-bit code at this segment
; (rb_up writes its base), and 16-bit data with real mode's 64K limit.
rx_gdtr     dw  rx_gdt_end - rx_gdt - 1
            dd  0                       ; its linear address, from rx_init

rx_gdt:
            dd  0, 0                    ; the null descriptor
            dw  0FFFFh                  ; 08h: limit 15-0
            dw  0                       ; base 15-0
            dw  9200h                   ; base 23-16, present writable data
            dw  00CFh                   ; limit 19-16, 4K granular, 32-bit
            dw  0FFFFh, 0, 9A00h, 00CFh ; 10h: flat code, 32-bit
            dw  0FFFFh, 0, 9A00h, 0000h ; 18h: code at this segment, 16-bit
            dw  0FFFFh, 0, 9200h, 0000h ; 20h: data, 64K, 16-bit
rx_gdt_end:

; And the one INT 15h AH=87h wants, in the BIOS's own layout: null,
; the table's own descriptor, source, destination, and two the BIOS
; fills in for itself.
rx_gdt87    db  48 dup(0)

rx_handles  db  (RX_MAXH * RXH_LEN) dup(0)

rx_tab:
        dw  OFFSET rxf00                ; get version
        dw  OFFSET rxf01                ; request the HMA
        dw  OFFSET rxf02                ; release the HMA
        dw  OFFSET rxf03                ; global enable A20
        dw  OFFSET rxf04                ; global disable A20
        dw  OFFSET rxf05                ; local enable A20
        dw  OFFSET rxf06                ; local disable A20
        dw  OFFSET rxf07                ; query A20
        dw  OFFSET rxf08                ; query free extended memory
        dw  OFFSET rxf09                ; allocate
        dw  OFFSET rxf0A                ; free
        dw  OFFSET rxf0B                ; move
        dw  OFFSET rxf0C                ; lock
        dw  OFFSET rxf0D                ; unlock
        dw  OFFSET rxf0E                ; get handle information
        dw  OFFSET rxf0F                ; reallocate
        dw  OFFSET rxf10                ; request a UMB
        dw  OFFSET rxf11                ; release a UMB
        dw  OFFSET rxf12                ; reallocate a UMB
