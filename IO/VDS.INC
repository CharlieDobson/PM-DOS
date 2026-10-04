;=====================================================================
; VDS.INC - Virtual DMA Services, INT 4Bh AH=81h
;
; A card that moves its own data - a bus-master network or SCSI
; adapter, which on PCI is most of them - is given the address of a
; buffer and puts that number on the bus.  It has to be the PHYSICAL
; address, and a real-mode driver has only one way to work an address
; out: segment times sixteen, plus offset.  That is right as long as
; the page tables say nothing, and PAGING.INC spends its whole header
; on the two places where they say something:
;
;   UPPER MEMORY.  A driver loaded high by DEVICEHIGH= is running in
;   pages that are somewhere above 1MB and only APPEAR at C8000h.
;   Segment times sixteen is the address of a hole.
;
;   THE EXPANDED MEMORY PAGE FRAME, which is the same thing four times
;   over and moves when a program asks it to.
;
; A driver that hands its card C8000h has told it to write into the
; ROM area, or into nothing.  This is the interface every such driver
; already knows how to ask - EMM386 had the same problem for the same
; reason - and a driver decides whether to ask by looking at bit 5 of
; the byte at 40:7Bh, which is set here.
;
; WHAT IS ANSWERED, and it is the half of the specification that tells
; the truth about memory:
;
;   8102h  the version, and what this provider can do
;   8103h  lock a region: where is it, physically, and is it in one
;          piece
;   8104h  unlock it again, which is nothing - no page here ever moves
;   8105h  the same for a region in several pieces: a list of them, or
;          of page table entries
;   8106h  and unlock that
;   810Bh, 810Ch  translation off and on for a DMA channel, for a
;          driver that hands the 8237 what 8103h told it (VDMA.INC)
;
; WHAT IS REFUSED is the half that lends memory out.  8107h to 810Ah
; are a buffer of the provider's own, below 16MB and in one piece, for
; a region that is neither; this provider has no such buffer, says so
; (8102h reports its size as zero), and fails the four calls with the
; specification's own codes.  A region that is not in one piece is
; reported as that - with how much of it IS, which is what lets a
; driver move its data a piece at a time - and is never silently
; copied somewhere else.
;
; A DRIVER THAT DOES NOT ASK, and programs the ISA DMA controller with
; segment times sixteen from upper memory, is put right by the monitor
; standing in for the controller's ports - VDMA.INC, which uses vd_run
; below for the same question.  Its bounce buffer is the monitor's own
; and is not lent out here.
;
; REACHED BY A HLT, like the mouse: the vector points at a stub below
; 1MB, the monitor stops on it, and vd_hlt is asked whether the HLT
; was this one.  Anything on INT 4Bh that is not AH=81h goes to
; whatever had the vector before - a SCSI adapter's ROM uses it for a
; service of its own - or returns unchanged if nothing did.
;
; Included by IOPM.ASM, after VM86.INC.
;=====================================================================

VD_VECTOR       EQU     4Bh
VD_BDAFLAG      EQU     BDA_BASE + 7Bh          ; bit 5: VDS is here
VD_VERSION      EQU     0100h                   ; the specification's 1.00
VD_PRODUCT      EQU     5044h                   ; 'PD'
VD_REVISION     EQU     0001h

; The errors, as the specification numbers them
VDE_NOTCONTIG   EQU     01h                     ; not in one piece
VDE_ALIGN       EQU     02h                     ; crosses a 64K or 128K line
VDE_NOBUF       EQU     04h                     ; no buffer to lend
VDE_BADREGION   EQU     07h                     ; not memory at all
VDE_TABLE       EQU     09h                     ; more pieces than entries
VDE_BADBUF      EQU     0Ah                     ; no such buffer
VDE_UNSUP       EQU     0Fh                     ; no such function

; What DX asks for
VDF_64K         EQU     0010h                   ; must not cross a 64K line
VDF_128K        EQU     0020h                   ; ...or a 128K one
VDF_PTES        EQU     0040h                   ; 8105h: page table entries

; The descriptor ES:DI points at
DDS_SIZE        EQU     00h                     ; dd - bytes
DDS_OFFSET      EQU     04h                     ; dd
DDS_SEG         EQU     08h                     ; dw - and the segment
DDS_BUFID       EQU     0Ah                     ; dw - 0: no buffer was used
DDS_PHYS        EQU     0Ch                     ; dd - 8103h's answer
EDDS_AVAIL      EQU     0Ch                     ; dw - entries there is room for
EDDS_USED       EQU     0Eh                     ; dw - entries filled in
EDDS_TABLE      EQU     10h                     ; and the entries

;=====================================================================
; vd_setup - AX = the stub's segment, from IOS_BUSINIT.  0: there is
;            no low memory for one, and no VDS.
;
; The vector is set ONCE, like every other the kernel plants: the
; table is the machine's, and a resident program that hooked INT 4Bh
; must still be hooked when the next program starts.
;=====================================================================
vd_setup:
        pushad
        movzx   eax,ax
        mov     dword ptr [vd_seg],eax
        or      eax,eax
        jz      vdu_none
        mov     edi,eax
        shl     edi,4
        mov     word ptr [edi],0CFF4h           ; HLT, and an IRET nothing
                                                ; should ever reach
        mov     ebx,dword ptr ds:[VD_VECTOR * 4]
        mov     dword ptr [vd_old],ebx
        shl     eax,16                          ; seg:0000
        mov     dword ptr ds:[VD_VECTOR * 4],eax
        mov     dword ptr [vd_mine],eax
        or      byte ptr ds:[VD_BDAFLAG],20h
vdu_none:
        popad
        ret

;=====================================================================
; vd_park - AL = 0: the machine is about to be handed to real mode,
;           where a HLT is a HLT and segment times sixteen is the
;           truth; AL = 1: it has come back.
;=====================================================================
vd_park:
        pushad
        cmp     dword ptr [vd_seg],0
        je      vdk_out
        or      al,al
        jnz     vdk_back
        and     byte ptr ds:[VD_BDAFLAG],NOT 20h
        mov     eax,dword ptr ds:[VD_VECTOR * 4]
        mov     dword ptr [vd_parked],eax       ; somebody may have hooked it
        mov     eax,dword ptr [vd_old]
        mov     dword ptr ds:[VD_VECTOR * 4],eax
        jmp     short vdk_out
vdk_back:
        mov     eax,dword ptr [vd_parked]
        mov     dword ptr ds:[VD_VECTOR * 4],eax
        or      byte ptr ds:[VD_BDAFLAG],20h
vdk_out:
        popad
        ret

;=====================================================================
; vd_rmdown - rm_int is about to run a BIOS call in real mode.  Bit 5
;             of 40:7Bh comes off and INT 4Bh goes back to the BIOS's
;             own for the length of the call.
; vd_rmup   - and both are put back as they were.
;
; A BUS-MASTER ROM ASKS VDS TOO.  An Adaptec 154x's INT 13h code
; reads 40:7Bh before it hands the card an address, and with bit 5
; set it calls INT 4Bh to lock its buffer.  In a box that is right.
; Down in rm_int it lands on the stub's HLT in real mode, where a HLT
; is a HLT: the CPU waits for the next tick, the IRET after it returns
; nothing, and the ROM carries on with a descriptor nobody filled in.
; Every PM-DOS before this one hung on its first read of a disk that
; such a card's BIOS serves - before CONFIG.SYS, before the kernel had
; read a sector of anything.
;
; Nothing in real mode needs the translation anyway: paging is off
; down there, so segment times sixteen is the physical address.  Only
; bit 5 is put back, since the rest of 40:7Bh is the BIOS's.
;=====================================================================
vd_rmdown:
        cmp     dword ptr [vd_seg],0
        je      vrd_out
        push    eax
        mov     al,byte ptr ds:[VD_BDAFLAG]
        and     al,20h
        mov     byte ptr [vd_rmflag],al
        and     byte ptr ds:[VD_BDAFLAG],NOT 20h
        mov     eax,dword ptr ds:[VD_VECTOR * 4]
        mov     dword ptr [vd_rmvec],eax
        mov     eax,dword ptr [vd_old]
        mov     dword ptr ds:[VD_VECTOR * 4],eax
        pop     eax
vrd_out:
        ret

vd_rmup:
        cmp     dword ptr [vd_seg],0
        je      vru_out
        push    eax
        mov     eax,dword ptr [vd_rmvec]
        mov     dword ptr ds:[VD_VECTOR * 4],eax
        mov     al,byte ptr [vd_rmflag]
        or      byte ptr ds:[VD_BDAFLAG],al
        pop     eax
vru_out:
        ret

;=====================================================================
; vd_freeze - EDI -> the copy of the vector table BIOS calls will run
;             with (rm_freeze, RMCALL.INC).  Its INT 4Bh becomes what
;             vd_rmdown puts at 0:0 for a call, and the rest of what
;             vd_rmdown does - bit 5 of 40:7Bh - it goes on doing.
;             Everything preserved.
;=====================================================================
vd_freeze:
        cmp     dword ptr [vd_seg],0
        je      vdz_out
        push    eax
        mov     eax,dword ptr [vd_old]
        mov     dword ptr [edi + VD_VECTOR * 4],eax
        pop     eax
vdz_out:
        ret

;=====================================================================
; vd_hlt - ESI -> a box's context, stopped at a HLT.  CF clear if the
;          HLT was the INT 4Bh stub's, and the call has been answered.
;=====================================================================
vd_hlt:
        push    eax
        mov     eax,dword ptr [vd_seg]
        or      eax,eax
        jz      vdh_no
        cmp     dword ptr [esi+V86C_CS],eax
        jne     vdh_no
        cmp     word ptr [esi+V86C_EIP],0
        jne     vdh_no
        pop     eax
        pushad
        cmp     byte ptr [esi+V86C_EAX+1],81h
        je      vdh_ours

        ; Somebody else's INT 4Bh.  The frame the INT pushed is still
        ; on the guest's stack, so sending the guest to the old
        ; handler is the whole of chaining to it: its IRET returns to
        ; the caller as though this stub had never been in the way.
        mov     eax,dword ptr [vd_old]
        or      eax,eax
        jz      vdh_iret
        movzx   edx,ax
        mov     dword ptr [esi+V86C_EIP],edx
        shr     eax,16
        mov     dword ptr [esi+V86C_CS],eax
        jmp     short vdh_done
vdh_iret:
        call    vd_frame
        jmp     short vdh_done

vdh_ours:
        call    vd_frame                ; the caller's own flags, and back
                                        ; after its INT
        movzx   edi,word ptr [esi+V86C_ES]
        shl     edi,4
        movzx   eax,word ptr [esi+V86C_EDI]
        add     edi,eax                 ; EDI -> the descriptor
        movzx   edx,word ptr [esi+V86C_EDX]     ; EDX = the flags
        mov     al,byte ptr [esi+V86C_EAX]
        cmp     al,02h
        je      vdf_version
        cmp     al,03h
        je      vdf_lock
        cmp     al,04h
        je      vdf_ok
        cmp     al,05h
        je      vdf_scatter
        cmp     al,06h
        je      vdf_ok
        cmp     al,07h
        je      vdf_nobuf
        cmp     al,08h
        je      vdf_badbuf
        cmp     al,09h
        je      vdf_badbuf
        cmp     al,0Ah
        je      vdf_badbuf
        cmp     al,0Bh
        je      vdf_disable
        cmp     al,0Ch
        je      vdf_enable
        mov     al,VDE_UNSUP
vdf_fail:
        mov     byte ptr [esi+V86C_EAX],al
        or      dword ptr [esi+V86C_FLAGS],1
        jmp     short vdh_done
vdf_ok:
        and     dword ptr [esi+V86C_FLAGS],NOT 1
vdh_done:
        popad
        clc
        ret
vdh_no:
        pop     eax
        stc
        ret

vdf_nobuf:
        mov     al,VDE_NOBUF
        jmp     short vdf_fail
vdf_badbuf:
        mov     al,VDE_BADBUF
        jmp     short vdf_fail

;--- 810Bh and 810Ch: translation off and on again for DMA channel BX.
;--- There is translation to turn off now (VDMA.INC): a driver that has
;--- asked where its buffer is hands the controller that answer, and
;--- these are how it says so.  Counted, as the specification counts
;--- them, and 810Ch sets the zero flag when the count is back to zero.
vdf_disable:
        mov     al,1
        jmp     short vdf_xlat
vdf_enable:
        xor     al,al
vdf_xlat:
        movzx   ebx,word ptr [esi+V86C_EBX]
        call    vdma_noxl
        jc      vdf_fail
        jz      vdf_zf
        and     dword ptr [esi+V86C_FLAGS],NOT 40h
        jmp     vdf_ok
vdf_zf:
        or      dword ptr [esi+V86C_FLAGS],40h
        jmp     vdf_ok

;--- 8102h: who is here.  DX bit 3 - "all memory is in one piece" - is
;--- NOT set, because upper memory is not; SI:DI, the size of the
;--- buffer there is to lend, is zero.
vdf_version:
        mov     word ptr [esi+V86C_EAX],VD_VERSION
        mov     word ptr [esi+V86C_EBX],VD_PRODUCT
        mov     word ptr [esi+V86C_ECX],VD_REVISION
        mov     word ptr [esi+V86C_EDX],0
        mov     word ptr [esi+V86C_ESI],0
        mov     word ptr [esi+V86C_EDI],0
        jmp     short vdf_ok

;--- 8103h: lock a region.  The answer is its physical address, if it
;--- is in one piece and crosses no line it was told not to.  If it is
;--- not, the size in the descriptor becomes how much of it is - which
;--- is what the specification asks for and what a driver that means
;--- to carry on needs.
vdf_lock:
        mov     ecx,DDS_PHYS + 4        ; the answer goes back into it
        call    vd_ddsok
        jc      vdf_badreg
        call    vd_region               ; EBX = linear, ECX = bytes
        call    vd_run                  ; EAX = physical, ECX = unbroken
        jc      vdf_badreg
        mov     dword ptr [edi+DDS_PHYS],eax
        mov     word ptr [edi+DDS_BUFID],0
        cmp     ecx,dword ptr [edi+DDS_SIZE]
        jae     vdl_whole
        mov     dword ptr [edi+DDS_SIZE],ecx
        mov     al,VDE_NOTCONTIG
        jmp     vdf_fail
vdl_whole:
        mov     ecx,dword ptr [edi+DDS_SIZE]
        mov     ebx,0FFFFh
        test    edx,VDF_64K
        jnz     vdl_line
        mov     ebx,1FFFFh
        test    edx,VDF_128K
        jz      vdf_ok
vdl_line:
        and     eax,ebx                 ; how far into its 64K or 128K
        inc     ebx
        sub     ebx,eax                 ; ...and how much of that is left
        cmp     ecx,ebx
        jbe     vdf_ok
        mov     dword ptr [edi+DDS_SIZE],ebx
        mov     al,VDE_ALIGN
        jmp     vdf_fail
vdf_badreg:
        mov     al,VDE_BADREGION
        jmp     vdf_fail

;--- 8105h: the same for a region that may be in pieces.  A list of
;--- them - address and length, eight bytes a piece - or, if DX bit 6
;--- asks, a page table entry for every page with the offset into the
;--- first of them in BX.  If the table is too short the count says how
;--- long it needed to be and the size says how much the entries there
;--- was room for do cover.
vdf_scatter:
        call    vd_region
        mov     dword ptr [vd_left],ecx
        mov     dword ptr [vd_done],0
        movzx   eax,word ptr [edi+EDDS_AVAIL]
        mov     dword ptr [vd_avail],eax
        lea     ecx,[eax*8 + EDDS_TABLE]        ; as long as the guest says
        test    edx,VDF_PTES                    ; its table is, which is up
        jz      vds_room                        ; to half a megabyte past
        lea     ecx,[eax*4 + EDDS_TABLE]        ; FFFF:FFFF
vds_room:
        call    vd_ddsok
        jc      vdf_badreg
        xor     ebp,ebp                 ; EBP = entries so far
        test    edx,VDF_PTES
        jnz     vds_ptes
vds_piece:
        cmp     dword ptr [vd_left],0
        je      vds_end
        mov     ecx,dword ptr [vd_left]
        call    vd_run
        jc      vdf_badreg
        cmp     ebp,dword ptr [vd_avail]
        jae     vds_nowrite
        mov     dword ptr [edi+EDDS_TABLE+ebp*8],eax
        mov     dword ptr [edi+EDDS_TABLE+ebp*8+4],ecx
        add     dword ptr [vd_done],ecx
vds_nowrite:
        inc     ebp
        add     ebx,ecx
        sub     dword ptr [vd_left],ecx
        jmp     short vds_piece

vds_ptes:
        mov     eax,ebx
        and     eax,0FFFh
        mov     word ptr [esi+V86C_EBX],ax
vds_page:
        cmp     dword ptr [vd_left],0
        je      vds_end
        mov     ecx,1
        call    vd_run
        jc      vdf_badreg
        mov     ecx,ebx                 ; what is left of this page
        and     ecx,0FFFh
        neg     ecx
        add     ecx,1000h
        cmp     ecx,dword ptr [vd_left]
        jbe     vds_pagelen
        mov     ecx,dword ptr [vd_left]
vds_pagelen:
        cmp     ebp,dword ptr [vd_avail]
        jae     vds_pnowrite
        and     eax,0FFFFF000h
        or      eax,1                   ; present, and staying where it is
        mov     dword ptr [edi+EDDS_TABLE+ebp*4],eax
        add     dword ptr [vd_done],ecx
vds_pnowrite:
        inc     ebp
        add     ebx,ecx
        sub     dword ptr [vd_left],ecx
        jmp     short vds_page

vds_end:
        mov     word ptr [edi+EDDS_USED],bp
        cmp     ebp,dword ptr [vd_avail]
        jbe     vdf_ok
        mov     eax,dword ptr [vd_done]
        mov     dword ptr [edi+DDS_SIZE],eax
        mov     al,VDE_TABLE
        jmp     vdf_fail

;---------------------------------------------------------------------
; vd_frame - ESI -> the context.  Take the frame the INT pushed off
;            the guest's stack, as the handler's IRET would have.
;---------------------------------------------------------------------
vd_frame:
        push    eax
        push    ebx
        push    edx
        mov     ebx,dword ptr [esi+V86C_SS]
        shl     ebx,4
        movzx   edx,word ptr [esi+V86C_ESP]
        movzx   eax,word ptr [ebx+edx]
        mov     dword ptr [esi+V86C_EIP],eax
        add     dx,2
        movzx   eax,word ptr [ebx+edx]
        mov     dword ptr [esi+V86C_CS],eax
        add     dx,2
        movzx   eax,word ptr [ebx+edx]
        mov     word ptr [esi+V86C_FLAGS],ax
        add     dx,2
        mov     word ptr [esi+V86C_ESP],dx
        pop     edx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; vd_region - EDI -> a descriptor -> EBX = the linear address it
;             names, ECX = its length.
;---------------------------------------------------------------------
;---------------------------------------------------------------------
; vd_ddsok - EDI -> the guest's descriptor, ECX = how many bytes of it
;            the answer will write.  CF unless the guest could have
;            written them itself.
;
; ES:DI is the guest's and so is the count of table entries, and the
; answer is written as ring 0: an 8105h with its descriptor at
; FFFF:0000 and room claimed for 65535 pieces was half a megabyte of
; physical addresses written into PMDOS.SYS.  Everything preserved.
;---------------------------------------------------------------------
vd_ddsok:
        push    eax
        mov     eax,edi
        call    usr_wok
        pop     eax
        ret

vd_region:
        movzx   ebx,word ptr [edi+DDS_SEG]
        shl     ebx,4
        add     ebx,dword ptr [edi+DDS_OFFSET]
        mov     ecx,dword ptr [edi+DDS_SIZE]
        ret

;=====================================================================
; vd_run - BUS_PHYSOF.  EBX = a linear address, ECX = bytes wanted ->
;          EAX = the physical address, ECX = how many of those bytes
;          follow on from it without a break (all of them, if it is in
;          one piece).  CF if the first byte is not mapped at all.
;          Nothing else is changed.
;
; The page tables are read and nothing is assumed, so this is right
; about upper memory, about the page frame wherever it points today,
; and about the HMA with the A20 gate shut - which is the first 64K of
; the machine, and would be a surprising place for a card to write.
;=====================================================================
vd_run:
        push    ebx
        push    edx
        push    esi
        push    edi
        mov     esi,ebx
        call    vd_phys                 ; EAX = the first byte, physically
        jc      vdr_no
        mov     edi,eax                 ; EDI = the answer
        mov     edx,ebx
        and     edx,0FFFh
        neg     edx
        add     edx,1000h               ; EDX = bytes so far: to the end
                                        ; of the first page
vdr_more:
        cmp     edx,ecx
        jae     vdr_all
        mov     ebx,esi
        add     ebx,edx                 ; the page after
        call    vd_phys
        jc      vdr_part
        push    edi
        add     edi,edx
        cmp     eax,edi
        pop     edi
        jne     vdr_part
        add     edx,1000h
        jmp     short vdr_more
vdr_all:
        mov     edx,ecx
vdr_part:
        mov     ecx,edx
        mov     eax,edi
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        clc
        ret
vdr_no:
        xor     ecx,ecx
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; vd_phys - EBX = a linear address -> EAX = the physical one.  CF if
;           nothing is mapped there.
;---------------------------------------------------------------------
vd_phys:
        push    edx
        mov     eax,ebx
        shr     eax,22
        mov     edx,dword ptr [PG_DIR + eax*4]
        test    edx,1
        jz      vdp_no
        test    edx,80h                 ; a 4MB page: the entry is the answer
        jz      vdp_table
        and     edx,0FFC00000h
        mov     eax,ebx
        and     eax,003FFFFFh
        or      eax,edx
        pop     edx
        clc
        ret
vdp_table:
        and     edx,0FFFFF000h
        mov     eax,ebx
        shr     eax,12
        and     eax,3FFh
        mov     edx,dword ptr [edx + eax*4]
        test    edx,1
        jz      vdp_no
        and     edx,0FFFFF000h
        mov     eax,ebx
        and     eax,0FFFh
        or      eax,edx
        pop     edx
        clc
        ret
vdp_no:
        pop     edx
        stc
        ret

;=====================================================================
; State
;=====================================================================
vd_seg          dd      0                       ; the stub's segment
vd_old          dd      0                       ; INT 4Bh as the BIOS left it
vd_mine         dd      0                       ; ...and as it was made
vd_parked       dd      0                       ; ...and as it was at a park
vd_rmvec        dd      0                       ; ...and during an rm_int
vd_rmflag       db      0, 0, 0, 0              ; 40:7Bh bit 5 during an rm_int
vd_left         dd      0
vd_done         dd      0
vd_avail        dd      0
