;=====================================================================
; VBE.INC - a graphics mode for a native program, and the linear frame
;           buffer to draw it in: 21h/F1h AL=1Fh
;
; A NATIVE PROGRAM HAS NO BIOS AND CANNOT REACH VIDEO MEMORY.  Both are
; the point of ring 3 (RING3.INC): its segment is its slot and nothing
; else.  21h/F0h does a text screen's work for it; this does a graphics
; screen's, through VBE 2.0.  The calls are listed with the VBF_*
; equates in PMDOS.INC.
;
; THE BIOS IS CALLED THROUGH THE THUNK, with XFER_BUF as the block it
; answers into, and what it answers is copied out to the program.  The
; thunk runs with the vector table rm_freeze took when CONFIG.SYS was
; finished, so a VBE extender has to be loaded from CONFIG.SYS (INSTALL=)
; to be seen here - one loaded from AUTOEXEC.BAT serves the DOS boxes
; only.  4F00h's far pointers point into XFER_BUF or into the ROM, and
; neither is the program's, so VBF_INFO copies what they point at into
; the program's own buffer and points at that instead.
;
; THE FRAME BUFFER IS A SELECTOR, SEL_VBE, AND NOT AN ADDRESS.  A
; pointer in the program's segment can only name its slot; the frame
; buffer is near the top of the address space, and a descriptor based
; on it, DPL 3 and as long as video memory, is the only way to give the
; program that and nothing else.  So a program draws through ES - REP
; MOVSD from a picture in its own memory is the natural shape - or a
; far pointer.  ES, because DS and ES are the two segment registers
; every DOS call gives back: FS and GS come back null from any call
; that went to the BIOS, VBF_SET included (rmc_back loads them with the
; kernel's selector, and the return to ring 3 drops it).
;
; Video memory is mapped where it is (IOS_MAPPHYS), as DPMI 0800h maps
; it, so the identity PAGING.INC keeps holds here too.  All of it is
; described, not one screen, so that a program can draw the next page
; while 4F07h shows this one; when the processor has no 4MB pages and
; the pool of device tables will not stretch that far, one screen.
;
; ONLY THE LINEAR FRAME BUFFER.  A VBE mode set here must ask for it
; (VBM_LFB): a banked mode would want 4F05h for every 64K drawn, and
; each one a trip to real mode.  VGA modes are allowed for mode 13h's
; sake, which gets the 64K window at A0000h - and for the text modes,
; which are how a program goes back to one.
;
; WHAT A PROGRAM SETS, ITS EXIT PUTS BACK.  4Ch, and a fault, call
; vbe_exit for the program that is ending: the shell would otherwise
; come back writing text into a graphics screen.  It is kept by EXEC
; depth, because a child may set a mode while its parent has one: each
; depth remembers the mode that was there when it first set one, and
; its exit sets that again - the parent's mode, with SEL_VBE describing
; the parent's frame buffer, or the text mode the first of them found,
; with its font if it had 43 or 50 lines.  The screen's contents are
; not kept; a mode set clears them, as it does under DOS.
;
; SEL_VBE IS NEVER TAKEN AWAY, ONLY EMPTIED (PMDOS.INC says why): a
; program that has finished with it may still have it in ES.
;
; Included by DOSMAIN.ASM, last: it describes SEL_VBE with RING3.INC's
; r3_wrdesc.
;=====================================================================

VGA_GFXWIN      EQU     000A0000h               ; the graphics modes' window
VGA_DACRD       EQU     3C7h                    ; the entry to read from
VGA_DACWR       EQU     3C8h                    ; ...and to write to
VGA_DACDATA     EQU     3C9h                    ; red, green, blue in turn

;=====================================================================
; fF1_vbe - 21h/F1h AL=1Fh, the function in bits 16-23 of EAX.
;=====================================================================
fF1_vbe:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     i21_unsup
        movzx   eax,byte ptr [ebp+F_EAX+2]
        cmp     eax,VBF_INFO
        je      vin_go
        cmp     eax,VBF_MODE
        je      vmq_go
        cmp     eax,VBF_SET
        je      vsm_go
        cmp     eax,VBF_RESTORE
        je      vrs_go
        cmp     eax,VBF_PALETTE
        je      vpl_go
        cmp     eax,VBF_BIOS
        je      vbb_go
        jmp     i21_unsup

;---------------------------------------------------------------------
; VBF_INFO - EDI -> VBI_LEN bytes.  -> EAX = the version, ECX = modes.
;
; The block as the BIOS gave it, up to its reserved area; past that it
; is this routine's: the mode list at VBI_RESERVED and the strings at
; VBI_OEMDATA, VBE_STRLEN bytes each, and the far pointers turned into
; the program's own pointers to them.  A VBE 1.x block has no vendor,
; product or revision - those bytes were reserved then and are not
; pointers - so they come back 0.
;---------------------------------------------------------------------
vin_go:
        mov     eax,[ebp+F_EDI]
        mov     ecx,VBI_LEN
        call    cbuf_ok
        jc      buf_deny
        call    vbe_ctl                         ; the block, in XFER_BUF
        jc      i21_fail
        mov     edi,[ebp+F_EDI]
        push    edi
        xor     eax,eax
        mov     ecx,VBI_LEN / 4
        rep     stosd
        pop     edi
        push    edi
        mov     esi,XFER_BUF
        mov     ecx,VBI_RESERVED
        rep     movsb
        pop     edi
        mov     edx,edi                         ; EDX = the buffer, as the
        sub     edx,[r3_base]                   ; program names it

        ; The modes.  The list may be in the ROM, or in the very block
        ; it came in - this copies it out of XFER_BUF either way.
        mov     eax,dword ptr [edi+VBI_MODES]
        call    vbe_flat
        mov     esi,eax                         ; 0 for no list at all
        lea     eax,[edx+VBI_RESERVED]
        mov     dword ptr [edi+VBI_MODES],eax
        lea     ebx,[edi+VBI_RESERVED]
        xor     ecx,ecx
        or      esi,esi
        jz      vin_lend
vin_list:
        mov     ax,word ptr [esi]
        cmp     ax,0FFFFh
        je      vin_lend
        cmp     ecx,VBE_MAXMODES                ; more than fit: the rest
        jae     vin_lend                        ; are left off
        mov     word ptr [ebx+ecx*2],ax
        add     esi,2
        inc     ecx
        jmp     vin_list
vin_lend:
        mov     word ptr [ebx+ecx*2],0FFFFh
        mov     [ebp+F_ECX],ecx

        ; The strings.
        mov     esi,VBI_OEMSTR
        mov     ebx,VBI_OEMDATA
        call    vin_str
        cmp     word ptr [edi+VBI_VERSION],0200h
        jb      vin_old
        mov     esi,VBI_VENDOR
        add     ebx,VBE_STRLEN
        call    vin_str
        mov     esi,VBI_PRODUCT
        add     ebx,VBE_STRLEN
        call    vin_str
        mov     esi,VBI_PRODREV
        add     ebx,VBE_STRLEN
        call    vin_str
        jmp     vin_done
vin_old:
        xor     eax,eax
        mov     word ptr [edi+VBI_OEMREV],ax
        mov     dword ptr [edi+VBI_VENDOR],eax
        mov     dword ptr [edi+VBI_PRODUCT],eax
        mov     dword ptr [edi+VBI_PRODREV],eax
vin_done:
        movzx   eax,word ptr [edi+VBI_VERSION]
        mov     [ebp+F_EAX],eax
        jmp     i21_ok

;--- vin_str - EDI -> the program's block, EDX = the block as the
;--- program names it, ESI = the offset of a far pointer in it, EBX =
;--- where in it the string goes.  The string is copied there and the
;--- pointer pointed at the copy; a null pointer stays null.  Everything
;--- preserved.
vin_str:
        pushad
        add     esi,edi                         ; ESI -> the field
        mov     eax,dword ptr [esi]
        mov     dword ptr [esi],0
        call    vbe_flat
        jz      vis_out
        add     edx,ebx
        mov     dword ptr [esi],edx
        mov     esi,eax
        add     edi,ebx
        mov     ecx,VBE_STRLEN - 1              ; the block was zeroed, so
vis_copy:                                       ; the terminator is there
        lodsb
        or      al,al
        jz      vis_out
        stosb
        loop    vis_copy
vis_out:
        popad
        ret

;---------------------------------------------------------------------
; VBF_MODE - CX = a mode, EDI -> VMI_LEN bytes: 4F01h's block.
;---------------------------------------------------------------------
vmq_go:
        mov     eax,[ebp+F_EDI]
        mov     ecx,VMI_LEN
        call    cbuf_ok
        jc      buf_deny
        call    vbe_ctl
        jc      i21_fail
        movzx   ecx,word ptr [ebp+F_ECX]
        call    vbe_minfo
        jc      i21_fail
        mov     esi,XFER_BUF
        mov     edi,[ebp+F_EDI]
        mov     ecx,VMI_LEN / 4
        rep     movsd
        jmp     i21_ok

;---------------------------------------------------------------------
; VBF_SET - EBX = the mode -> EAX = SEL_VBE, ECX = the bytes it covers,
;           EDX = bytes per scan line.
;
; The mode is checked here and set by vbe_apply.  Only when it has been
; set is anything remembered: the mode that was there before, if this
; is the program's first, and the mode it now is - without VBM_KEEP,
; because when it is set again it is set for a program that will draw
; it again.
;---------------------------------------------------------------------
vsm_go:
        mov     ebx,[ebp+F_EBX]
        test    ebx,NOT (VBM_MODE OR VBM_LFB OR VBM_KEEP)
        jnz     vsm_bad                         ; 4F02h's others want a
                                                ; block of CRTC timings
        mov     eax,ebx
        and     eax,VBM_MODE
        cmp     eax,100h
        jae     vsm_vbe
        test    ebx,VBM_LFB                     ; a VGA mode has no linear
        jnz     vsm_bad                         ; frame buffer to ask for
        jmp     vsm_ok
vsm_vbe:
        test    ebx,VBM_LFB                     ; and a VBE mode has nothing
        jz      vsm_bad                         ; else here
vsm_ok:
        cmp     word ptr [vbe_cur],VBE_NONE
        jne     vsm_found
        call    vbe_savetext                    ; the screen as it is found
vsm_found:
        call    vbe_apply
        jc      i21_fail
        and     ebx,NOT VBM_KEEP
        mov     esi,[r3_depth]
        cmp     byte ptr [vbe_dset+esi],0
        jne     vsm_noted
        mov     byte ptr [vbe_dset+esi],1
        push    eax
        mov     ax,word ptr [vbe_cur]
        mov     word ptr [vbe_prev+esi*2],ax
        pop     eax
vsm_noted:
        mov     word ptr [vbe_cur],bx
        mov     [ebp+F_EAX],eax
        mov     [ebp+F_ECX],ecx
        mov     [ebp+F_EDX],edx
        jmp     i21_ok
vsm_bad:
        mov     ax,ERR_BADDATA
        jmp     i21_fail

;---------------------------------------------------------------------
; VBF_RESTORE - what this program's exit would do, now.
;---------------------------------------------------------------------
vrs_go:
        mov     eax,[r3_depth]
        call    vbe_exit
        jmp     i21_ok

;---------------------------------------------------------------------
; VBF_PALETTE - BL = 00h or 80h to set, 01h to read; ECX entries from
;               EDX; EDI -> four bytes an entry.
;
; 4F09h first, because in a mode that is not VGA-compatible it is the
; only way there is.  When the BIOS has not got it - a VBE 1.2 BIOS, or
; one with no VBE at all, in mode 13h - the DAC is a VGA's and its
; ports are the way, with the bytes in the DAC's order rather than
; 4F09h's.  80h's wait for the retrace is the BIOS's; the ports do not
; wait.
;---------------------------------------------------------------------
vpl_go:
        mov     ecx,[ebp+F_ECX]
        mov     edx,[ebp+F_EDX]
        mov     ax,ERR_BADDATA
        or      ecx,ecx
        jz      i21_fail
        cmp     ecx,256
        ja      i21_fail
        cmp     edx,256
        jae     i21_fail
        lea     ebx,[ecx+edx]
        cmp     ebx,256
        ja      i21_fail
        mov     bl,byte ptr [ebp+F_EBX]
        cmp     bl,01h
        je      vpl_size
        test    bl,7Fh                          ; 00h or 80h
        jnz     i21_fail
vpl_size:
        push    ecx
        mov     eax,[ebp+F_EDI]
        shl     ecx,2
        call    cbuf_ok
        pop     ecx
        jc      buf_deny
        cmp     bl,01h
        je      vpl_read

        mov     esi,[ebp+F_EDI]
        mov     edi,XFER_BUF
        push    ecx
        rep     movsd
        pop     ecx
        call    vpl_bios
        jnc     i21_ok
        mov     esi,XFER_BUF
        mov     eax,edx
        mov     edx,VGA_DACWR
        out     dx,al
        mov     edx,VGA_DACDATA
vpl_wr:
        mov     al,byte ptr [esi+2]             ; red
        out     dx,al
        mov     al,byte ptr [esi+1]             ; green
        out     dx,al
        mov     al,byte ptr [esi]               ; blue
        out     dx,al
        add     esi,4
        loop    vpl_wr
        jmp     i21_ok

vpl_read:
        call    vpl_bios
        jnc     vpl_give
        mov     edi,XFER_BUF
        push    ecx
        mov     eax,edx
        mov     edx,VGA_DACRD
        out     dx,al
        mov     edx,VGA_DACDATA
vpl_rd:
        in      al,dx
        mov     byte ptr [edi+2],al
        in      al,dx
        mov     byte ptr [edi+1],al
        in      al,dx
        mov     byte ptr [edi],al
        mov     byte ptr [edi+3],0
        add     edi,4
        loop    vpl_rd
        pop     ecx
vpl_give:
        mov     esi,XFER_BUF
        mov     edi,[ebp+F_EDI]
        rep     movsd
        jmp     i21_ok

;--- vpl_bios - 4F09h: BL, ECX entries from EDX, the entries at
;--- XFER_BUF.  CF if the BIOS has not got it.  Everything preserved.
vpl_bios:
        push    eax
        mov     eax,4F09h
        call    vbe_frame
        movzx   eax,bl
        mov     dword ptr ds:[RMR_EBX],eax
        mov     dword ptr ds:[RMR_ECX],ecx
        mov     dword ptr ds:[RMR_EDX],edx
        call    vbe_int10
        pop     eax
        ret

;---------------------------------------------------------------------
; VBF_BIOS - bits 24-31 of EAX = 03h, 06h, 07h or 08h.
;
; The VBE functions a program may have as they are: they take numbers
; and give numbers back, so there is nothing to copy and nothing the
; kernel keeps that they could make wrong.  Not 02h (VBF_SET), 04h (a
; buffer of state), 05h (banks: see the top), 09h (VBF_PALETTE) or 0Ah
; (code for a program that can reach the BIOS's segment).
;---------------------------------------------------------------------
vbb_go:
        movzx   eax,byte ptr [ebp+F_EAX+3]
        cmp     al,03h
        je      vbb_ask
        cmp     al,06h
        jb      i21_unsup
        cmp     al,08h
        ja      i21_unsup
vbb_ask:
        call    vbe_ctl
        jc      i21_fail
        add     eax,4F00h
        call    vbe_frame
        mov     eax,[ebp+F_EBX]
        mov     dword ptr ds:[RMR_EBX],eax
        mov     eax,[ebp+F_ECX]
        mov     dword ptr ds:[RMR_ECX],eax
        mov     eax,[ebp+F_EDX]
        mov     dword ptr ds:[RMR_EDX],eax
        call    vbe_int10
        mov     ax,ERR_VBEFAIL
        jc      i21_fail
        mov     eax,dword ptr ds:[RMR_EBX]
        mov     [ebp+F_EBX],eax
        mov     eax,dword ptr ds:[RMR_ECX]
        mov     [ebp+F_ECX],eax
        mov     eax,dword ptr ds:[RMR_EDX]
        mov     [ebp+F_EDX],eax
        jmp     i21_ok

;=====================================================================
; vbe_exit - EAX = an EXEC depth.  If the program there set a mode and
;            has not put it back, put back the one it found.  For 4Ch,
;            a fault and VBF_RESTORE.  Everything preserved.
;
; The one it found was either a parent's - which is set again, frame
; buffer and all, so that the parent's SEL_VBE still describes what it
; is drawing in - or the text mode the first of them found.  If the
; parent's will not set again, which it did once, text is better than
; whatever is on the screen.
;=====================================================================
vbe_exit:
        pushad
        cmp     eax,EXEC_MAX
        ja      vex_out
        cmp     byte ptr [vbe_dset+eax],0
        je      vex_out
        mov     byte ptr [vbe_dset+eax],0
        movzx   ebx,word ptr [vbe_prev+eax*2]
        call    vbe_apply
        jnc     vex_set
        mov     ebx,VBE_NONE
        call    vbe_apply
vex_set:
        mov     word ptr [vbe_cur],bx
vex_out:
        popad
        ret

;=====================================================================
; vbe_apply - EBX = a mode as VBF_SET takes it, or VBE_NONE for the
;             text mode the first VBF_SET found.  Set it, and describe
;             SEL_VBE for it.  -> EAX = SEL_VBE or 0, ECX = bytes, EDX =
;             bytes per scan line; CF and AX = the error if the mode
;             could not be had.  EBX, ESI, EDI and EBP preserved.
;
; NOTHING CHANGES UNTIL THE MODE IS SET.  The frame buffer is mapped
; first, because a mode that is set and cannot be drawn in is worse
; than one that was refused; and SEL_VBE is described last, so that a
; refusal leaves it describing the mode that is still on the screen.
;=====================================================================
vbe_apply:
        push    ebx
        push    esi
        push    edi
        cmp     bx,VBE_NONE
        je      vap_text
        mov     eax,ebx
        and     eax,VBM_MODE
        cmp     eax,100h
        jae     vap_vbe

        ; A VGA MODE, by INT 10h AH=00h - which says nothing back, so
        ; the BIOS data area is asked whether it took.
        test    ebx,VBM_KEEP
        jz      vap_vga
        or      al,80h
vap_vga:
        call    vbe_frame
        push    eax
        mov     al,10h
        call    dword ptr ds:[IOS_REALINT]
        pop     eax
        and     al,7Fh
        mov     ah,byte ptr ds:[BDA_VMODE]
        and     ah,7Fh
        cmp     al,ah
        jne     vap_refused
        cmp     al,3                            ; the text modes have no
        jbe     vap_none                        ; window: 21h/F0h is how a
        cmp     al,7                            ; native program writes text
        je      vap_none
        mov     ebx,VGA_GFXWIN
        mov     ecx,10000h
        cmp     al,6
        ja      vap_window
        mov     ebx,VGA_TEXT                    ; 4 to 6, the CGA's, are at
        mov     ecx,8000h                       ; B8000h
vap_window:
        call    vbe_desc
        xor     edx,edx                         ; planar, or 13h's 320: the
        mov     eax,SEL_VBE                     ; mode's own rules
        jmp     vap_ok

        ; THE TEXT MODE, as it was found, and the font that gave it more
        ; than 25 lines: the 8x8 for 43 or 50, the 8x14 for 28.
vap_text:
        movzx   eax,byte ptr [vbe_txtmode]
        call    vbe_frame
        mov     al,10h
        call    dword ptr ds:[IOS_REALINT]
        mov     al,byte ptr [vbe_txtmode]
        cmp     al,3
        jbe     vap_rows
        cmp     al,7
        jne     vap_none                        ; a graphics mode, found
vap_rows:
        mov     al,byte ptr [vbe_txtrows]
        cmp     al,25
        jbe     vap_none
        mov     eax,1112h                       ; BL = 0: font block 0
        cmp     byte ptr [vbe_txtrows],40
        jae     vap_font
        mov     eax,1111h
vap_font:
        call    vbe_frame
        mov     al,10h
        call    dword ptr ds:[IOS_REALINT]
vap_none:
        call    vbe_empty
        xor     eax,eax
        xor     ecx,ecx
        xor     edx,edx
        jmp     vap_ok

        ; A VBE MODE, with its linear frame buffer.
vap_vbe:
        mov     dword ptr [vbe_want],ebx
        mov     esi,eax                         ; the mode number
        call    vbe_ctl
        jc      vap_fail                        ; AX = ERR_NOVBE
        mov     ax,ERR_VBEFAIL
        cmp     word ptr [vbe_ver],0200h        ; no linear buffer before 2.0
        jb      vap_fail
        mov     ecx,esi
        call    vbe_minfo
        jc      vap_fail
        mov     ax,ERR_VBEFAIL
        mov     cx,word ptr ds:[XFER_BUF+VMI_ATTR]
        and     cx,VMA_HW OR VMA_LFB
        cmp     cx,VMA_HW OR VMA_LFB
        jne     vap_fail
        mov     ebx,dword ptr ds:[XFER_BUF+VMI_PHYS]
        or      ebx,ebx
        jz      vap_fail

        ; Bytes per scan line - VBE 3.0 gives the linear buffer's its own
        ; field, which may differ from the banked one.
        movzx   edx,word ptr ds:[XFER_BUF+VMI_PITCH]
        cmp     word ptr [vbe_ver],0300h
        jb      vap_pitch
        movzx   ecx,word ptr ds:[XFER_BUF+VMI_LINPITCH]
        jecxz   vap_pitch
        mov     edx,ecx
vap_pitch:
        movzx   edi,word ptr ds:[XFER_BUF+VMI_YRES]
        imul    edi,edx                         ; EDI = one screen
        movzx   ecx,word ptr [vbe_mem64]
        shl     ecx,16                          ; ECX = all of video memory
        cmp     ecx,edi
        jae     vap_most
        mov     ecx,edi
vap_most:
        mov     eax,ebx                         ; and not past 4GB
        add     eax,ecx
        jnc     vap_map
        mov     ecx,ebx
        neg     ecx
vap_map:
        call    dword ptr ds:[IOS_MAPPHYS]
        jnc     vap_mapped
        mov     ecx,edi
        call    dword ptr ds:[IOS_MAPPHYS]
        mov     ax,ERR_NOMEM
        jc      vap_fail
vap_mapped:
        push    ecx
        mov     eax,4F02h
        call    vbe_frame
        mov     eax,dword ptr [vbe_want]
        mov     dword ptr ds:[RMR_EBX],eax
        call    vbe_int10
        pop     ecx
        mov     ax,ERR_VBEFAIL
        jc      vap_fail
        call    vbe_desc
        mov     eax,SEL_VBE
vap_ok:
        pop     edi
        pop     esi
        pop     ebx
        clc
        ret
vap_refused:
        mov     ax,ERR_VBEFAIL
vap_fail:
        pop     edi
        pop     esi
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; vbe_desc - SEL_VBE over EBX for ECX bytes.  -> ECX = the bytes it
;            covers, which past 1MB is whole pages: the limit counts in
;            them there (r3_setdesc says why).  Everything else
;            preserved.
;---------------------------------------------------------------------
vbe_desc:
        push    eax
        push    edx
        push    edi
        xor     dh,dh
        cmp     ecx,100000h
        jbe     vds_write
        add     ecx,0FFFh
        and     ecx,NOT 0FFFh
        mov     dh,DGRAN_G
vds_write:
        push    ecx
        or      dh,dh
        jz      vds_bytes
        shr     ecx,12
vds_bytes:
        dec     ecx
        mov     edi,GDT_BASE + GDT_IDX_VBE * 8
        mov     dl,DACC_DATA
        call    r3_wrdesc
        pop     ecx
        pop     edi
        pop     edx
        pop     eax
        ret

;--- vbe_empty - SEL_VBE as it is with no mode set: present, and empty
;--- (expand-down, limit 4GB).  Everything preserved.
vbe_empty:
        mov     dword ptr ds:[GDT_BASE + GDT_IDX_VBE * 8],0000FFFFh
        mov     dword ptr ds:[GDT_BASE + GDT_IDX_VBE * 8 + 4],00CFF600h
        ret

;--- vbe_savetext - the screen as the first VBF_SET found it, for
;--- vbe_apply to go back to.  Everything preserved.
vbe_savetext:
        push    eax
        mov     al,byte ptr ds:[BDA_VMODE]
        and     al,7Fh
        mov     byte ptr [vbe_txtmode],al
        mov     al,byte ptr ds:[BDA_VROWS]
        inc     al
        mov     byte ptr [vbe_txtrows],al
        pop     eax
        ret

;---------------------------------------------------------------------
; vbe_ctl - 4F00h into XFER_BUF, asked for as VBE 2.0.  vbe_ver and
;           vbe_mem64 are set from it; CF and AX = ERR_NOVBE if there is
;           no VBE.  Everything else preserved.
;
; A kilobyte is cleared and not the 512 bytes the block is, because
; there are BIOSes that write past it.
;---------------------------------------------------------------------
vbe_ctl:
        pushad
        mov     edi,XFER_BUF
        xor     eax,eax
        mov     ecx,VBI_LEN * 2 / 4
        rep     stosd
        mov     dword ptr ds:[XFER_BUF+VBI_SIG],32454256h       ; 'VBE2'
        mov     eax,4F00h
        call    vbe_frame
        call    vbe_int10
        jc      vct_none
        cmp     dword ptr ds:[XFER_BUF+VBI_SIG],41534556h       ; 'VESA'
        jne     vct_none
        mov     ax,word ptr ds:[XFER_BUF+VBI_VERSION]
        mov     word ptr [vbe_ver],ax
        mov     ax,word ptr ds:[XFER_BUF+VBI_MEMORY]
        mov     word ptr [vbe_mem64],ax
        popad
        clc
        ret
vct_none:
        popad
        mov     ax,ERR_NOVBE
        stc
        ret

;--- vbe_minfo - ECX = a mode: 4F01h's block for it into XFER_BUF.  CF
;--- and AX = ERR_VBEFAIL if the BIOS will not.  Everything else
;--- preserved.
vbe_minfo:
        pushad
        mov     edi,XFER_BUF
        push    ecx
        xor     eax,eax
        mov     ecx,VMI_LEN * 2 / 4
        rep     stosd
        pop     ecx
        mov     eax,4F01h
        call    vbe_frame
        mov     dword ptr ds:[RMR_ECX],ecx
        call    vbe_int10
        popad
        jnc     vmn_ok
        mov     ax,ERR_VBEFAIL
vmn_ok:
        ret

;--- vbe_frame - RMFRAME cleared, EAX in it, and ES:DI = XFER_BUF.
;--- Everything preserved.
vbe_frame:
        push    eax
        push    ecx
        push    edi
        push    eax
        mov     edi,RMFRAME
        xor     eax,eax
        mov     ecx,RMFRAME_LEN / 4
        rep     stosd
        pop     eax
        mov     dword ptr ds:[RMR_EAX],eax
        mov     word ptr ds:[RMR_ES],XFER_SEG
        pop     edi
        pop     ecx
        pop     eax
        ret

;--- vbe_int10 - INT 10h with RMFRAME as it stands.  CF unless AX came
;--- back 004Fh, which is VBE's "supported, and done".  Everything
;--- preserved.
vbe_int10:
        push    eax
        mov     al,10h
        call    dword ptr ds:[IOS_REALINT]
        pop     eax
        cmp     word ptr ds:[RMR_EAX],004Fh
        jne     vi1_no
        clc
        ret
vi1_no:
        stc
        ret

;--- vbe_flat - EAX = a real-mode far pointer -> EAX = the address it
;--- names, and ZF if it was null.
vbe_flat:
        push    edx
        movzx   edx,ax
        shr     eax,16
        shl     eax,4
        add     eax,edx
        pop     edx
        ret

;=====================================================================
; Data
;=====================================================================
vbe_want    dd  0                       ; the mode vap_vbe is setting
vbe_cur     dw  VBE_NONE                ; the mode VBF_SET last set
vbe_ver     dw  0                       ; 4F00h's version, last asked
vbe_mem64   dw  0                       ; ...and its video memory, in 64K
vbe_prev    dw  EXEC_MAX + 1 dup (VBE_NONE)     ; by EXEC depth: vbe_cur
                                        ; when it first set a mode
vbe_dset    db  EXEC_MAX + 1 dup (0)    ; ...and whether it has one set
vbe_txtmode db  3                       ; the screen the first VBF_SET
vbe_txtrows db  25                      ; found
