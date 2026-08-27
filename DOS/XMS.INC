;=====================================================================
; XMS.INC - the extended memory manager, for 16-bit guests
;
; This is HIMEM.SYS's job, done by the kernel instead of by a driver,
; and the reason is structural rather than a preference.  A real
; HIMEM's two jobs are to switch the CPU into protected mode to move
; blocks and to own extended memory.  It can do neither here: the
; machine is ALREADY in protected mode, a guest runs at CPL 3 in V86
; and cannot leave it, and extended memory has belonged to ARENA.INC
; since before the guest existed.  Every VDM ever written - Windows,
; OS/2, DOSEMU - emulates XMS for those same two reasons.
;
; It is also less work than the alternative.  The hard parts of a real
; XMS driver are the mode switching and the block move; here a block
; move is a REP MOVSD between two flat addresses, and "lock a handle
; and tell me its physical address" is a question we can answer
; honestly because linear IS physical.
;
; HOW A PROGRAM FINDS IT.  INT 2Fh AX=4300h answers AL=80h, and
; AX=4310h hands back a far pointer that the program CALLs.  It never
; goes through an interrupt again - which is the awkward part of
; providing XMS from outside the DOS box, because the entry point has
; to be sixteen-bit code at a real-mode address.
;
; THE ENTRY POINT IS A HLT.  Two bytes of guest-visible memory holding
; HLT/RETF: the HLT faults out of V86 with the fault address in the
; context, the VM loop recognises the address as ours, serves the
; call, steps IP past the HLT, and the RETF takes the program home.
; This is the standard VDM callback trick (DOSEMU's "magic" opcode is
; the same idea) and it beats claiming an interrupt vector, because
; every vector belongs to somebody: PM-DOS's own CDD uses INT 60h.
; The block is a TABLE of them, so EMS and DPMI can have entries of
; their own later without inventing a second mechanism.
;
; WHAT IT DELIBERATELY DOES NOT OFFER:
;   the HMA   - 100000h is real memory here and nothing is using it,
;               but handing out the 64K a program would run its own
;               code in means honouring A20 for code fetches as well
;               as data, and 01h says "no HMA" instead.  DOS=HIGH is
;               the only common customer and PM-DOS has no DOS=HIGH.
;   UMBs      - it does now, when DOS=UMB has built some.  10h-12h
;               allocate out of the same MCB chain 48h does, because
;               two doors onto one pool is correct and two pools is a
;               bug waiting to be found by whoever opens both.
;
; Included by DOSMAIN.ASM.
;=====================================================================

XMS_MAXH        EQU     32              ; handles, which is DOS's own default

; The callback block used to be described here, because XMS was the
; only thing in it.  It is XMSCB_ENTRY, VDMCB_IRET and the DPMI
; entries, and it now lives in PMDOS.INC with the rest of the layout
; the whole kernel agrees on - CRITERR.INC needs VDMCB_IRET and is
; assembled before this file, so an EQU here is an EQU too late.

XH_FLAGS        EQU     0               ; dd - 0 free, 1 allocated
XH_KB           EQU     4               ; dd - size in KB
XH_ADDR         EQU     8               ; dd - flat address of the block
XH_LOCK         EQU     12              ; dd - lock count
XH_OWN          EQU     16              ; dd - the owner id that took it
XH_LEN          EQU     20

; Error codes, from the XMS 3.0 specification
XSE_NOTIMPL     EQU     80h
XSE_VDISK       EQU     81h
XSE_A20         EQU     82h
XSE_NOMEM       EQU     0A0h
XSE_NOHANDLE    EQU     0A1h
XSE_BADHANDLE   EQU     0A2h
XSE_BADSRCH     EQU     0A3h
XSE_BADSRCO     EQU     0A4h
XSE_BADDSTH     EQU     0A5h
XSE_BADDSTO     EQU     0A6h
XSE_BADLEN      EQU     0A7h
XSE_OVERLAP     EQU     0A8h
XSE_PARITY      EQU     0A9h
XSE_NOTLOCKED   EQU     0AAh
XSE_LOCKED      EQU     0ABh
XSE_LOCKCOUNT   EQU     0ACh
XSE_LOCKFAIL    EQU     0ADh
XSE_NOHMA       EQU     90h
XSE_HMAINUSE    EQU     91h
XSE_NOUMB       EQU     0B0h
XSE_NOUMBS      EQU     0B1h
XSE_BADUMB      EQU     0B2h

;=====================================================================
; xms_init - plant the callback block in conventional memory.
;
; It is allocated from the DOS arena like anything else and marked
; owner 8, which is what DOS itself used for memory belonging to the
; system: no PSP ever equals 8, so the exit sweeps walk straight past
; it and it lives as long as the machine does.  A real XMS driver
; costs conventional memory too, so MEM showing a paragraph gone is
; the truth and not an accounting error.
;=====================================================================
xms_init:
        pushad
        mov     ebx,1                           ; one paragraph is plenty
        call    lm_sysalloc
        jc      xi_none
        mov     word ptr ds:[xms_cbseg],ax

        movzx   edi,ax
        shl     edi,4
        mov     byte ptr [edi+XMSCB_ENTRY],0F4h ; HLT
        mov     byte ptr [edi+XMSCB_ENTRY+1],0CBh       ; RETF
        mov     byte ptr [edi+DPMICB_ENTRY],0F4h        ; and DPMI's, which
        mov     byte ptr [edi+DPMICB_ENTRY+1],0CBh      ; never gets as far as
                                                        ; the RETF when it works
        mov     byte ptr [edi+DPMICB_SIM],0CDh  ; INT ...
        mov     byte ptr [edi+DPMICB_SIM+1],3   ; ... patched per call
        mov     byte ptr [edi+DPMICB_RET],0F4h  ; and the landing pad the
                                                ; handler's IRET returns to

xi_none:
        popad
        ret

;=====================================================================
; xms_int2F - INT 2Fh from a guest.  ESI is unused; the context is
;             vdm_ctx as everywhere else in the bridge.
;
; CF clear on return = handled;  CF set = not ours, give it back to
; whoever the guest has in its own IVT.  THAT SECOND CASE IS THE
; IMPORTANT ONE: 2Fh is the DOS multiplex interrupt, every TSR ever
; written hooks it, and a monitor that claims the vector and swallows
; what it does not recognise breaks all of them silently.
;=====================================================================
xms_int2F:
        mov     ax,word ptr [vdm_ctx+V86C_EAX]
        cmp     ax,4300h
        je      x2f_present
        cmp     ax,4310h
        je      x2f_entry
        stc                                     ; not ours
        ret

x2f_present:
        cmp     word ptr ds:[xms_cbseg],0
        je      x2f_no                          ; no callback, no XMS
        mov     byte ptr [vdm_ctx+V86C_EAX],80h ; AL = 80h: installed
        clc
        ret
x2f_no:
        stc
        ret

x2f_entry:
        movzx   eax,word ptr ds:[xms_cbseg]
        or      eax,eax
        jz      x2f_no
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        mov     word ptr [vdm_ctx+V86C_EBX],XMSCB_ENTRY
        clc
        ret

;=====================================================================
; xms_iscb - CF clear if the context's CS:IP is sitting on one of our
;            callback bytes, i.e. this HLT is a service call and not a
;            program that really did halt.
;=====================================================================
xms_iscb:
        push    eax
        push    edx
        movzx   eax,word ptr ds:[xms_cbseg]
        or      eax,eax
        jz      xcb_no
        cmp     dword ptr [vdm_ctx+V86C_CS],eax
        jne     xcb_no
        mov     edx,dword ptr [vdm_ctx+V86C_EIP]
        cmp     edx,XMSCB_ENTRY
        jne     xcb_no
        pop     edx
        pop     eax
        clc
        ret
xcb_no:
        pop     edx
        pop     eax
        stc
        ret

;=====================================================================
; xms_call - serve one XMS function.  AH selects, as the spec says.
;
; Every function answers AX=1 for success, or AX=0 with BL holding a
; reason.  Registers are 16-bit halves of the context throughout,
; except the 3.0 calls that deliberately use the full 32.
;=====================================================================
xms_call:
        ; Step over the HLT first, so that every exit below lands on
        ; the RETF whether it succeeded or not.
        inc     dword ptr [vdm_ctx+V86C_EIP]

        movzx   eax,byte ptr [vdm_ctx+V86C_EAX+1]
        cmp     eax,XMS_MAXFN
        ja      xc_ext
        jmp     dword ptr [xms_table+eax*4]
xc_ext:
        cmp     eax,88h
        je      xf_88
        cmp     eax,89h
        je      xf_89
        jmp     xms_notimpl

;---------------------------------------------------------------------
; Shared exits.  xms_ok is AX=1; xms_err takes the code in BL.
;---------------------------------------------------------------------
xms_ok:
        mov     word ptr [vdm_ctx+V86C_EAX],1
        ret
xms_err:                                        ; BL = why
        mov     word ptr [vdm_ctx+V86C_EAX],0
        mov     byte ptr [vdm_ctx+V86C_EBX],bl
        ret
xms_notimpl:
        mov     bl,XSE_NOTIMPL
        jmp     xms_err

;---------------------------------------------------------------------
; 00h - get version.  AX = XMS version in BCD, BX = our own revision,
;       DX = 1 if there is an HMA.  There is not; see the note at the
;       top of the file.  3.00 is claimed because the 32-bit calls
;       (88h, 89h) are here, and without them a machine with more than
;       64MB has to report its free memory saturated at 65535 KB the
;       way 21h/36h saturates its clusters.
;---------------------------------------------------------------------
xf_00:
        mov     word ptr [vdm_ctx+V86C_EAX],0300h
        mov     word ptr [vdm_ctx+V86C_EBX],0029h       ; PM-DOS 0.29
        mov     word ptr [vdm_ctx+V86C_EDX],0
        ret

;---------------------------------------------------------------------
; 01h/02h - request and release the HMA.  Refused, always, and with
;           the code that says the machine has not got one rather than
;           the code that says somebody else got there first.
;---------------------------------------------------------------------
xf_01:
xf_02:
        mov     bl,XSE_NOHMA
        jmp     xms_err

;---------------------------------------------------------------------
; 03h-07h - the A20 gate.
;
; Global enable and disable set it outright.  The LOCAL pair are
; counted, because they exist for a driver that needs A20 briefly
; inside a routine and must not clobber the setting of whoever called
; it - it is a lock, not a switch, and the count is the whole point.
;---------------------------------------------------------------------
xf_03:                                          ; global enable
        mov     byte ptr ds:[xms_a20g],1
        call    xms_a20set
        jmp     xms_ok
xf_04:                                          ; global disable
        cmp     dword ptr ds:[xms_a20cnt],0
        jne     xf_04_held                      ; somebody has it locked
        mov     byte ptr ds:[xms_a20g],0
        call    xms_a20set
        jmp     xms_ok
xf_04_held:
        mov     bl,XSE_A20
        jmp     xms_err

xf_05:                                          ; local enable
        inc     dword ptr ds:[xms_a20cnt]
        call    xms_a20set
        jmp     xms_ok
xf_06:                                          ; local disable
        cmp     dword ptr ds:[xms_a20cnt],0
        je      xf_06_none
        dec     dword ptr ds:[xms_a20cnt]
        call    xms_a20set
        jmp     xms_ok
xf_06_none:
        mov     bl,XSE_NOTLOCKED
        jmp     xms_err

xf_07:                                          ; query A20
        ; ASK THE GATE, not the bookkeeping.  xms_a20on is what this
        ; driver last set it to, and that is a different thing from
        ; what it is: a guest can move the gate itself through port
        ; 92h or the 8042, and the monitor lets it - the write is
        ; intercepted and honoured rather than refused.  Answering out
        ; of the shadow would have this function disagreeing with the
        ; program's own experiment two instructions later.
        call    dword ptr ds:[IOS_A20GET]
        movzx   eax,al
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        mov     byte ptr [vdm_ctx+V86C_EBX],0
        ret

;---------------------------------------------------------------------
; xms_a20set - push the gate to whatever the global setting and the
;              outstanding local locks add up to.
;
; It does not remember the answer.  It used to, for 07h to report, and
; that was wrong the moment the monitor started letting a guest move
; the gate itself: two records of one piece of state, and the one this
; driver kept was the one that could be stale.  07h asks the gate.
;---------------------------------------------------------------------
xms_a20set:
        push    eax
        mov     al,byte ptr ds:[xms_a20g]
        cmp     dword ptr ds:[xms_a20cnt],0
        je      xa20_have
        mov     al,1
xa20_have:
        call    dword ptr ds:[IOS_A20]
        pop     eax
        ret

;---------------------------------------------------------------------
; xms_a20reset - a fresh DOS box gets the gate a fresh PC has: shut.
;---------------------------------------------------------------------
xms_a20reset:
        mov     byte ptr ds:[xms_a20g],0
        mov     dword ptr ds:[xms_a20cnt],0
        call    xms_a20set
        ret

;---------------------------------------------------------------------
; 08h - query free extended memory.  AX = the largest block, DX = the
;       total, both in KB, both 16-bit and therefore both capped at
;       65535 - which is the XMS 2.0 interface showing its age, not a
;       shortcut here.  88h below is the answer to that.
;---------------------------------------------------------------------
xf_08:
        call    arena_avail
        shr     eax,10                          ; bytes -> KB
        call    xms_cap16
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        push    eax
        call    xms_total
        shr     eax,10
        call    xms_cap16
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        pop     eax
        or      ax,ax
        jz      xf_08_none
        mov     byte ptr [vdm_ctx+V86C_EBX],0
        ret
xf_08_none:
        mov     byte ptr [vdm_ctx+V86C_EBX],XSE_NOMEM
        ret

;---------------------------------------------------------------------
; 09h - allocate an extended memory block.  DX = KB wanted.
;---------------------------------------------------------------------
xf_09:
        movzx   ebx,word ptr [vdm_ctx+V86C_EDX]
        shl     ebx,10
        jmp     short xms_alloc_common
;---------------------------------------------------------------------
; 89h - the same thing with a 32-bit size in EDX, so that a program
;       can ask for more than 64MB.  XMS 3.0.
;---------------------------------------------------------------------
xf_89:
        mov     ebx,dword ptr [vdm_ctx+V86C_EDX]
        shl     ebx,10

xms_alloc_common:
        push    ebx
        call    xms_newh                        ; a handle first: running
        pop     ebx                             ; out of them after taking
        jc      xa_nohandle                     ; the memory is untidy
        push    esi
        or      ebx,ebx
        jnz     xa_size
        mov     ebx,16                          ; a zero-length block is
xa_size:                                        ; legal, and must not be
        call    arena_alloc                     ; a zero-length allocation
        jc      xa_nomem
        mov     dword ptr [esi+XH_FLAGS],1
        mov     dword ptr [esi+XH_ADDR],eax
        mov     eax,ebx
        shr     eax,10
        mov     dword ptr [esi+XH_KB],eax
        mov     dword ptr [esi+XH_LOCK],0
        mov     eax,dword ptr [cur_own]         ; whose it is, so that the
        mov     dword ptr [esi+XH_OWN],eax      ; end of one program in the
                                                ; box does not take a block
                                                ; belonging to another
        call    xms_handleno                    ; -> AX
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        pop     esi
        jmp     xms_ok
xa_nomem:
        mov     dword ptr [esi+XH_FLAGS],0      ; give the handle back
        pop     esi
        mov     bl,XSE_NOMEM
        jmp     xms_err
xa_nohandle:
        mov     bl,XSE_NOHANDLE
        jmp     xms_err

;---------------------------------------------------------------------
; 0Ah - free a block.  DX = handle.  A locked block is not freed: the
;       lock exists to promise somebody an address that stays put.
;---------------------------------------------------------------------
xf_0A:
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        call    xms_find
        jc      xf_0A_bad
        cmp     dword ptr [esi+XH_LOCK],0
        jne     xf_0A_locked
        mov     edx,dword ptr [esi+XH_ADDR]
        call    arena_free
        mov     dword ptr [esi+XH_FLAGS],0
        jmp     xms_ok
xf_0A_bad:
        mov     bl,XSE_BADHANDLE
        jmp     xms_err
xf_0A_locked:
        mov     bl,XSE_LOCKED
        jmp     xms_err

;---------------------------------------------------------------------
; 0Bh - move an extended memory block, and the reason XMS exists.
;
; DS:SI points at a sixteen-byte request: a length, then a source
; handle and offset, then a destination handle and offset.  A HANDLE
; OF ZERO MEANS THE OFFSET IS NOT AN OFFSET - it is a real-mode
; segment:offset far pointer into conventional memory, which is how a
; program gets its data into extended memory and back out again.
;
; Both ends resolve to flat addresses, and then it is one copy.  All
; the machinery a real driver needs for this - enabling A20, switching
; to protected mode, switching back - is machinery we are already
; standing in.
;---------------------------------------------------------------------
XMM_LEN         EQU     0               ; dd - bytes, and it must be even
XMM_SRCH        EQU     4               ; dw - handle, 0 = far pointer
XMM_SRCO        EQU     6               ; dd - offset, or seg:off
XMM_DSTH        EQU     10
XMM_DSTO        EQU     12

xf_0B:
        ; The request pointer goes in a static rather than a register.
        ; EBP is the INT 21h frame pointer everywhere else in this
        ; kernel, and borrowing it here would work today and break the
        ; first time one of these functions wants to call k21.
        movzx   eax,word ptr [vdm_ctx+V86C_DS]
        shl     eax,4
        movzx   edx,word ptr [vdm_ctx+V86C_ESI]
        add     eax,edx
        mov     dword ptr ds:[xms_mreq],eax

        mov     esi,eax
        mov     ecx,dword ptr [esi+XMM_LEN]
        test    ecx,1
        jnz     xf_0B_badlen
        mov     dword ptr ds:[xms_mlen],ecx

        movzx   eax,word ptr [esi+XMM_SRCH]
        mov     edx,dword ptr [esi+XMM_SRCO]
        call    xms_where
        jc      xf_0B_badsrc
        mov     dword ptr ds:[xms_msrc],eax

        mov     esi,dword ptr ds:[xms_mreq]
        movzx   eax,word ptr [esi+XMM_DSTH]
        mov     edx,dword ptr [esi+XMM_DSTO]
        call    xms_where
        jc      xf_0B_baddst
        mov     edi,eax
        mov     esi,dword ptr ds:[xms_msrc]
        mov     ecx,dword ptr ds:[xms_mlen]

        ; Overlap is legal and the direction has to follow it, exactly
        ; as a memmove does - the spec allows a block to be shuffled
        ; within itself and programs do it.
        jecxz   xf_0B_done
        cmp     edi,esi
        jbe     xf_0B_fwd
        mov     eax,esi
        add     eax,ecx
        cmp     edi,eax
        jae     xf_0B_fwd
        std                                     ; overlapping upwards
        add     esi,ecx
        dec     esi
        add     edi,ecx
        dec     edi
        rep     movsb
        cld
        jmp     short xf_0B_done
xf_0B_fwd:
        rep     movsb
xf_0B_done:
        jmp     xms_ok

xf_0B_badlen:
        mov     bl,XSE_BADLEN
        jmp     xms_err
xf_0B_badsrc:
        mov     bl,XSE_BADSRCH
        jmp     xms_err
xf_0B_baddst:
        mov     bl,XSE_BADDSTH
        jmp     xms_err

;---------------------------------------------------------------------
; xms_where - EAX = handle (0 = far pointer), EDX = offset or seg:off.
;             -> EAX = flat address, CF on a handle that is not ours.
;---------------------------------------------------------------------
xms_where:
        or      eax,eax
        jnz     xw_handle
        mov     eax,edx                         ; a real-mode far pointer:
        shr     eax,16                          ; segment in the high half
        shl     eax,4
        movzx   edx,dx
        add     eax,edx
        clc
        ret
xw_handle:
        push    esi
        call    xms_find
        jc      xw_bad
        mov     eax,dword ptr [esi+XH_ADDR]
        add     eax,edx
        pop     esi
        clc
        ret
xw_bad:
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; 0Ch/0Dh - lock and unlock.  Locking answers with the block's 32-bit
;           address in DX:BX, and here that address is the honest
;           physical one: the map is the identity, so there is nothing
;           to pin and nothing that could move.
;---------------------------------------------------------------------
xf_0C:
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        call    xms_find
        jc      xf_0C_bad
        inc     dword ptr [esi+XH_LOCK]
        mov     eax,dword ptr [esi+XH_ADDR]
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        shr     eax,16
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        jmp     xms_ok
xf_0C_bad:
        mov     bl,XSE_BADHANDLE
        jmp     xms_err

xf_0D:
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        call    xms_find
        jc      xf_0D_bad
        cmp     dword ptr [esi+XH_LOCK],0
        je      xf_0D_not
        dec     dword ptr [esi+XH_LOCK]
        jmp     xms_ok
xf_0D_bad:
        mov     bl,XSE_BADHANDLE
        jmp     xms_err
xf_0D_not:
        mov     bl,XSE_NOTLOCKED
        jmp     xms_err

;---------------------------------------------------------------------
; 0Eh - get handle information.  BH = lock count, BL = handles still
;       free, DX = the block's size in KB.
;---------------------------------------------------------------------
xf_0E:
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        call    xms_find
        jc      xf_0E_bad
        mov     eax,dword ptr [esi+XH_LOCK]
        mov     byte ptr [vdm_ctx+V86C_EBX+1],al
        call    xms_freeh
        mov     byte ptr [vdm_ctx+V86C_EBX],al
        mov     eax,dword ptr [esi+XH_KB]
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        jmp     xms_ok
xf_0E_bad:
        mov     bl,XSE_BADHANDLE
        jmp     xms_err

;---------------------------------------------------------------------
; 0Fh - resize a block.  BX = the new size in KB, DX = the handle.
;       A locked block cannot be resized, for the reason it cannot be
;       freed: somebody was promised the address it is at.
;---------------------------------------------------------------------
xf_0F:
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        call    xms_find
        jc      xf_0F_bad
        cmp     dword ptr [esi+XH_LOCK],0
        jne     xf_0F_locked
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        shl     ebx,10
        or      ebx,ebx
        jnz     xf_0F_sz
        mov     ebx,16
xf_0F_sz:
        mov     edx,dword ptr [esi+XH_ADDR]
        call    arena_resize
        jc      xf_0F_nomem
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]
        mov     dword ptr [esi+XH_KB],eax
        jmp     xms_ok
xf_0F_nomem:
        mov     bl,XSE_NOMEM
        jmp     xms_err
xf_0F_bad:
        mov     bl,XSE_BADHANDLE
        jmp     xms_err
xf_0F_locked:
        mov     bl,XSE_LOCKED
        jmp     xms_err

;---------------------------------------------------------------------
; 10h-12h - upper memory blocks.
;
; THE SAME MEMORY THE MCB CHAIN DESCRIBES, HANDED OUT THROUGH THE
; OTHER DOOR.  There is exactly one pool of upper memory here and two
; ways of asking for a piece of it, and they must not become two
; pools: a program that takes a UMB through XMS and one that takes it
; through 48h with a high strategy have to be unable to get the same
; paragraph.  So this allocates out of the chain, with the strategy
; bits that mean "upper only", and the block it hands back is an
; ordinary MCB that MEM will list and that 49h would also free.
;
; The relationship is the reverse of a real DOS's, and worth saying
; out loud: there, EMM386 owns the memory and DOS asks XMS for it with
; exactly this call, then lays a chain over what it gets.  Here the
; kernel is both, so the chain is authoritative and this is the
; wrapper.
;
; When there is no upper memory - no DOS=UMB, or nothing found - this
; answers B0h with DX = 0 exactly as it always did, which is the
; spec's "none at all".
;---------------------------------------------------------------------
xf_10:
        movzx   ebx,word ptr [vdm_ctx+V86C_EDX]
        or      ebx,ebx
        jz      xf_10_none                      ; nought paragraphs of it
        cmp     dword ptr [umb_have],0
        je      xf_10_none

        ; EDX AND NOT EAX, because AX is the answer by the time the
        ; strategy is put back - the same trap lm_alloc's own comment
        ; describes, and it reads as innocently here as it did there:
        ; the register looks free and the value has already arrived.
        mov     edx,[alloc_strat]
        push    edx
        mov     dword ptr [alloc_strat],80h     ; upper only, first fit
        call    lm_alloc
        pop     edx
        mov     [alloc_strat],edx
        jc      xf_10_small

        mov     dx,word ptr ds:[vdm_curpsp]     ; and it belongs to whoever
        call    lm_setown                       ; asked, so that 4Ch takes it
                                                ; back - see the note on 48h
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        movzx   eax,ax
        dec     eax
        shl     eax,4
        movzx   eax,word ptr [eax+RMCB_LEN]     ; what it really got
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        jmp     xms_ok

xf_10_small:
        mov     word ptr [vdm_ctx+V86C_EDX],bx  ; the largest there is
        mov     bl,XSE_NOUMB
        jmp     xms_err
xf_10_none:
        mov     word ptr [vdm_ctx+V86C_EDX],0
        mov     bl,XSE_NOUMBS
        jmp     xms_err

;--- 11h - release one.  DX = the segment it was given -----------------
xf_11:
        cmp     dword ptr [umb_have],0
        je      xf_11_bad
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        push    eax
        dec     eax
        shl     eax,4
        call    umb_isupper                     ; a conventional block is not
        pop     eax                             ; a UMB, whatever it says
        jz      xf_11_bad
        call    lm_free
        jc      xf_11_bad
        jmp     xms_ok
xf_11_bad:
        mov     bl,XSE_BADUMB
        jmp     xms_err

;--- 12h - resize one, XMS 3.0.  DX = segment, BX = new paragraphs ----
xf_12:
        cmp     dword ptr [umb_have],0
        je      xf_11_bad
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        push    eax
        dec     eax
        shl     eax,4
        call    umb_isupper
        pop     eax
        jz      xf_11_bad
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        call    lm_resize
        jc      xf_12_small
        jmp     xms_ok
xf_12_small:
        mov     word ptr [vdm_ctx+V86C_EDX],bx
        mov     bl,XSE_NOUMB
        jmp     xms_err

;---------------------------------------------------------------------
; 88h - query free extended memory, XMS 3.0.  EAX = the largest block
;       in KB, ECX = the highest address any block can end at, EDX =
;       the total free.  All 32-bit, which is the entire point of it.
;---------------------------------------------------------------------
xf_88:
        call    arena_avail
        shr     eax,10
        mov     dword ptr [vdm_ctx+V86C_EAX],eax
        call    xms_total
        shr     eax,10
        mov     dword ptr [vdm_ctx+V86C_EDX],eax
        mov     eax,dword ptr [arena_top]
        dec     eax
        mov     dword ptr [vdm_ctx+V86C_ECX],eax
        mov     byte ptr [vdm_ctx+V86C_EBX],0
        ret

;=====================================================================
; Handles.  A handle is an index into the table, plus one, so that
; zero stays available to mean "this offset is a far pointer" in the
; move request - which is a convention of the interface and not a
; choice made here.
;=====================================================================
;---------------------------------------------------------------------
; xms_newh - ESI -> a free handle slot, CF if there are none left.
;---------------------------------------------------------------------
xms_newh:
        push    ecx
        mov     esi,OFFSET xms_handles
        mov     ecx,XMS_MAXH
xnh_scan:
        cmp     dword ptr [esi+XH_FLAGS],0
        je      xnh_got
        add     esi,XH_LEN
        loop    xnh_scan
        pop     ecx
        stc
        ret
xnh_got:
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; xms_find - EAX = handle number.  ESI -> its slot, CF if it is not a
;            live handle.  A number out of range and a number naming a
;            freed handle are the same answer, deliberately: a program
;            that keeps using a handle after freeing it must be told.
;---------------------------------------------------------------------
xms_find:
        or      eax,eax
        jz      xfd_bad
        cmp     eax,XMS_MAXH
        ja      xfd_bad
        push    edx
        mov     edx,eax
        dec     edx
        imul    edx,XH_LEN
        mov     esi,OFFSET xms_handles
        add     esi,edx
        pop     edx
        cmp     dword ptr [esi+XH_FLAGS],0
        je      xfd_bad
        clc
        ret
xfd_bad:
        stc
        ret

;---------------------------------------------------------------------
; xms_handleno - ESI -> a slot.  AX = the handle number for it.
;---------------------------------------------------------------------
xms_handleno:
        mov     eax,esi
        sub     eax,OFFSET xms_handles
        shr     eax,4                           ; XH_LEN is 16, and a shift
        inc     eax                             ; leaves EDX and ECX alone
        ret                                     ; where a DIV would not

;---------------------------------------------------------------------
; xms_freeh - AL = how many handles are still free, saturated at 255.
;---------------------------------------------------------------------
xms_freeh:
        push    ecx
        push    esi
        xor     eax,eax
        mov     esi,OFFSET xms_handles
        mov     ecx,XMS_MAXH
xfh_scan:
        cmp     dword ptr [esi+XH_FLAGS],0
        jne     xfh_next
        inc     eax
xfh_next:
        add     esi,XH_LEN
        loop    xfh_scan
        pop     esi
        pop     ecx
        ret

;---------------------------------------------------------------------
; xms_total - EAX = every free byte in the arena added up, which is
;             not the same number as the largest single block and is
;             the one a program sizing a cache wants.
;---------------------------------------------------------------------
xms_total:
        push    ebx
        push    esi
        xor     eax,eax
        mov     esi,ARENA_BASE
xt_walk:
        cmp     dword ptr [esi+MCB_SIG],SIG_END
        je      xt_done
        cmp     dword ptr [esi+MCB_SIG],SIG_FREE
        jne     xt_next
        add     eax,dword ptr [esi+MCB_SIZE]
xt_next:
        mov     ebx,dword ptr [esi+MCB_SIZE]
        add     ebx,MCB_LEN
        add     esi,ebx
        cmp     esi,dword ptr [arena_top]
        jb      xt_walk
xt_done:
        pop     esi
        pop     ebx
        ret

;---------------------------------------------------------------------
; xms_cap16 - AX = EAX, or 65535 if it will not fit.  The 2.0 calls
;             report KB in sixteen bits and a big machine overflows
;             them; saturating is what real drivers do and what 21h/36h
;             already does with clusters.
;---------------------------------------------------------------------
xms_cap16:
        cmp     eax,0FFFFh
        jbe     xcap_ok
        mov     eax,0FFFFh
xcap_ok:
        ret

;=====================================================================
; xms_int15 - INT 15h from a guest.
;
; THIS IS THE HOLE XMS WOULD OTHERWISE LEAVE OPEN.  A program that
; ignores XMS altogether - and plenty did - asks the BIOS how much
; extended memory there is with AH=88h and then moves blocks into it
; with AH=87h.  Both are reflected straight to the real BIOS unless
; something stops them, and the real BIOS on this machine will
; cheerfully report every megabyte the kernel, PMIO and the arena are
; living in, after which AH=87h copies over them.
;
; The answer is the honest one and it is the same answer a machine
; with HIMEM loaded gives: NONE.  Extended memory is spoken for.  A
; program that wants some can ask through XMS like everybody else.
;
; Everything else on this vector is somebody else's - the mouse, the
; wait services, the joystick - and goes back to the guest's own IVT.
;=====================================================================
xms_int15:
        mov     ax,word ptr [vdm_ctx+V86C_EAX]
        cmp     ah,88h
        je      x15_88
        cmp     ah,87h
        je      x15_87
        cmp     ax,0E801h
        je      x15_e801
        cmp     ax,0E820h
        je      x15_e820
        cmp     ah,24h
        je      x15_24
        stc                                     ; not ours
        ret

;---------------------------------------------------------------------
; AH=24h - the BIOS A20 gate services, and the reason they are here is
; that they are the THIRD way to move the gate.  A program can write
; port 92h, it can talk to the 8042, or it can ask the BIOS to do one
; of those two on its behalf.  The first two are intercepted in the
; monitor; this is the third, and leaving it reflected would have the
; BIOS moving the real line behind the emulation's back - which is
; exactly the hole the other two were closed to stop.
;---------------------------------------------------------------------
x15_24:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,03h
        ja      x15_24bad
        cmp     al,02h
        je      x15_24get
        cmp     al,03h
        je      x15_24sup
        call    dword ptr ds:[IOS_A20]          ; AL = 0 disable, 1 enable
        mov     word ptr [vdm_ctx+V86C_EAX],0
        jmp     short x15_24ok

x15_24get:                                      ; AL = the current state
        call    dword ptr ds:[IOS_A20GET]
        mov     byte ptr [vdm_ctx+V86C_EAX],al
        mov     byte ptr [vdm_ctx+V86C_EAX+1],0
        mov     word ptr [vdm_ctx+V86C_ECX],0
        jmp     short x15_24ok

x15_24sup:                                      ; what routes exist
        ; Bit 0 = the keyboard controller, bit 1 = port 92h.  Both are
        ; claimed by the monitor, so both are true here whatever the
        ; underlying machine happens to have.
        mov     word ptr [vdm_ctx+V86C_EAX],0
        mov     word ptr [vdm_ctx+V86C_EBX],0003h
x15_24ok:
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        clc
        ret

x15_24bad:
        mov     byte ptr [vdm_ctx+V86C_EAX+1],86h
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        clc
        ret

x15_88:                                         ; KB of extended memory
        mov     word ptr [vdm_ctx+V86C_EAX],0
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        clc
        ret

x15_87:                                         ; block move
        ; Refused rather than implemented, and consistently: with 88h
        ; saying there are no kilobytes there is nowhere for a move to
        ; go.  AH=86h is the BIOS's own "no such function".
        mov     byte ptr [vdm_ctx+V86C_EAX+1],86h
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        clc
        ret

x15_e801:                                       ; 15M-and-above sizes
        mov     word ptr [vdm_ctx+V86C_EAX],0
        mov     word ptr [vdm_ctx+V86C_EBX],0
        mov     word ptr [vdm_ctx+V86C_ECX],0
        mov     word ptr [vdm_ctx+V86C_EDX],0
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        clc
        ret

x15_e820:                                       ; the memory map
        ; Refused outright.  Handing a DOS program a map of memory it
        ; cannot address, most of which is in use, would be worse than
        ; saying nothing - and a real machine of this vintage answers
        ; the same way.
        mov     byte ptr [vdm_ctx+V86C_EAX+1],86h
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        clc
        ret

;=====================================================================
; Data
;=====================================================================
XMS_MAXFN       EQU     12h

xms_table:
        dd      xf_00,     xf_01,     xf_02,     xf_03    ; 00-03
        dd      xf_04,     xf_05,     xf_06,     xf_07    ; 04-07
        dd      xf_08,     xf_09,     xf_0A,     xf_0B    ; 08-0B
        dd      xf_0C,     xf_0D,     xf_0E,     xf_0F    ; 0C-0F
        dd      xf_10,     xf_11,     xf_12               ; 10-12

xms_cbseg   dw  0                       ; the callback paragraph
            dw  0
xms_a20cnt  dd  0                       ; local enables outstanding
xms_a20g    db  0                       ; and the global setting
            db  0,0,0
xms_mreq    dd  0                       ; 0Bh's request block
xms_msrc    dd  0                       ; and its resolved ends
xms_mlen    dd  0
xms_handles db  (XMS_MAXH * XH_LEN) dup(0)

;=====================================================================
; xms_relown - the handles belonging to the program that just ended.
;
; The MEMORY is already accounted for: it came out of the arena with
; that program's owner id on it and vdm_cleanup's sweep takes it back.
; What would be left behind without this is the TABLE - handles still
; marked live, pointing at arena blocks that now belong to somebody
; else, waiting for the next program to find them.
;
; BY OWNER, not wholesale.  This used to empty the table on the
; grounds that there was one VM and therefore no live handles at the
; end of it.  Both halves of that stopped being true on the same day:
; a box can now hold a program and its children, and a TSR can outlive
; the box entirely - and its handles have to outlive it too, or the
; extended memory it is still using is handed to the next program.
;=====================================================================
xms_relown:
        pushad
        mov     edx,dword ptr [cur_own]
        mov     esi,OFFSET xms_handles
        mov     ecx,XMS_MAXH
xr_one:
        cmp     dword ptr [esi+XH_FLAGS],0
        je      xr_next
        cmp     dword ptr [esi+XH_OWN],edx
        jne     xr_next
        mov     dword ptr [esi+XH_FLAGS],0
        mov     dword ptr [esi+XH_LOCK],0
xr_next:
        add     esi,XH_LEN
        loop    xr_one
        popad
        ret
