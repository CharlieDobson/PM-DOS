;=====================================================================
; NDLOAD.INC - DEVICE= for a 32-BIT DRIVER
;
; What INC\NDRV.INC describes, done: a file whose D32 header says
; D32T_DRV is read whole into a block of the arena that is the
; kernel's for good, relocated to where it landed, zeroed past its
; image, checked for the table a driver begins with, given a row in
; nd_tab and told to INIT.  From then on it is called like the
; kernel's own drivers are - at a park, at a restart, and when a
; program asks for it through 21h/F1h AL=25h - and never freed.
;
; WHY NOT THE REAL-MODE LOADER'S ROAD.  DEVLOAD puts a driver below
; the megabyte, because a real-mode driver can reach nothing else,
; and reaches it through a V86 excursion with a request packet,
; because that is how one is spoken to.  A 32-bit driver needs
; neither, and giving it either would cost the two things it exists
; to save: conventional memory and the excursion.  What the two
; loaders share is the line - dl_split parses it and both read what
; it leaves in dl_path and dl_line - and the error codes, so that
; cfg_device prints the same five messages for both kinds.
;
; THE MEMORY IS THE KERNEL'S.  The block is allocated first fit, at
; CONFIG.SYS time, when the arena is nearly whole, and its owner is
; written as 0 whoever is running - which is what makes 4Ch's sweep
; leave it alone and park_xmsgrab never hand it to Windows (that
; gives away the largest FREE block and nothing allocated).  Nothing
; moves it afterwards: the relocation is done once and the address
; is in every pointer the driver keeps.
;
; THE RELOCATION IS CHECKED BY THE HEADER.  D32H_BASE is where the
; image was linked, and it is itself in the table - D32_DRVHEAD
; writes the header's own address there - so after the table has
; been applied the field must equal the block's address.  A table
; that was not this image's, or a header that was not written by the
; macro, fails that one comparison, and the file is refused before
; anything in it has been called.
;
; Included by DOSMAIN.ASM after DEVLOAD.INC, whose dl_split, dl_path
; and dl_line it uses.
;=====================================================================

ND_PROBELEN        EQU     D32_DRVSTUB + D32H_SIZE ; what is read to decide:
                                                ; a stub and the header
                                                ; behind it, or a bare
                                                ; header and a table

; WHAT A DRIVER HOLDS BESIDE ITS IMAGE, one record a row: nd_mem's row
; N is nd_tab's row N.  ndsv_alloc and ndsv_free keep it, NDF_MEM reads
; it.  The kernel's own bookkeeping and not part of the row a program
; is copied, so ND_LEN stays as every caller of NDF_INFO knows it.
NM_BYTES        EQU     0               ; dd - the blocks' payload, bytes
NM_BLOCKS       EQU     4               ; dd - how many blocks
NM_LEN          EQU     8

;=====================================================================
; nd_load - ESI -> the DEVICE= line.
;
;   CF clear:           a 32-bit driver, loaded and running.
;   CF set, EAX = 0:    not one of these - a real-mode driver, a file
;                       that is not there, or no path at all: DEVLOAD's
;                       business, and DEVLOAD's message.
;   CF set, EAX = DLERR_*: one of these that would not load, with the
;                       code cfg_device turns into a message.
;
; A D32 image of the wrong kind or version answers DLERR_NOFILE - "Bad
; or missing device driver" - rather than being handed to DEVLOAD,
; which would run its header as 16-bit code.
;=====================================================================
nd_load:
        pushad
        mov     dword ptr [nd_sft],0
        call    dl_split
        jc      ndl_notours
        mov     esi,OFFSET dl_path
        xor     eax,eax                 ; read only, deny none
        call    open_core
        jc      ndl_notours
        mov     [nd_sft],esi

        mov     eax,[esi+SFT_SIZE]
        mov     [nd_fsize],eax
        mov     ecx,eax
        cmp     ecx,ND_PROBELEN
        jbe     ndl_probe
        mov     ecx,ND_PROBELEN
ndl_probe:
        mov     edi,OFFSET nd_probe
        call    file_read
        jc      ndl_closenot
        mov     [nd_got],edx

        ; WHICH KIND OF FILE.  'CD' at byte zero is a bare image, which
        ; is what the linker writes and what nothing ships; the name
        ; DRVSTUB.ASM puts in its device header, with 'CD' behind the
        ; stub, is a driver as D32DRV wrote it.  Anything else is not
        ; ours.
        xor     ebx,ebx                 ; EBX = where the image starts
        cmp     dword ptr [nd_got],D32H_SIZE + NDT_LEN
        jb      ndl_closenot
        cmp     word ptr [nd_probe+D32H_SIG],D32_SIG
        je      ndl_image
        cmp     dword ptr [nd_got],ND_PROBELEN
        jb      ndl_closenot
        mov     esi,OFFSET nd_probe + DEVH_NAME
        mov     edi,OFFSET nd_stubname
        mov     ecx,8
        repe    cmpsb
        jne     ndl_closenot
        cmp     word ptr [nd_probe+D32_DRVSTUB+D32H_SIG],D32_SIG
        jne     ndl_closenot
        mov     ebx,D32_DRVSTUB
ndl_image:
        lea     esi,[nd_probe+ebx]      ; ESI -> the header, in the probe
        cmp     byte ptr [esi+D32H_TYPE],D32T_DRV
        jne     ndl_badfile
        cmp     byte ptr [esi+D32H_VER],D32_VER
        jne     ndl_badfile
        mov     [nd_skip],ebx

        ; THE SIZES, and every one of them is checked against the file
        ; before a byte is allocated.  The image runs from the header
        ; to the relocation table; the table runs to the end of the
        ; file; the BSS follows the image in memory, on a paragraph.
        mov     eax,[nd_fsize]
        sub     eax,ebx
        mov     [nd_imglen],eax         ; the image and its table
        mov     ecx,[esi+D32H_RELOC]
        mov     edx,[esi+D32H_NRELOC]
        mov     [nd_reloff],ecx
        mov     [nd_nreloc],edx
        cmp     ecx,D32H_SIZE + NDT_LEN
        jb      ndl_badfile             ; a table inside the header
        cmp     edx,10000000h
        jae     ndl_badfile             ; a count whose bytes would wrap
        shl     edx,2
        add     edx,ecx
        jc      ndl_badfile
        cmp     edx,eax
        ja      ndl_badfile             ; a table past the end of the file
        mov     eax,ecx
        add     eax,15
        and     eax,NOT 15
        mov     [nd_keep],eax           ; where the BSS begins
        add     eax,[esi+D32H_BSS]
        jc      ndl_badfile
        mov     [nd_total],eax          ; what stays resident

        ; THE BLOCK: the larger of what the file holds and what stays,
        ; since the table has to be read in before it is applied.
        ; First fit, whatever strategy a program set and left, so that
        ; a driver lands at the bottom of the arena like the next one.
        mov     ebx,[nd_imglen]
        cmp     ebx,eax
        jae     ndl_sized
        mov     ebx,eax
ndl_sized:
        mov     eax,[alloc_strat]
        push    eax
        mov     dword ptr [alloc_strat],AF_FIRST
        call    arena_alloc
        pop     dword ptr [alloc_strat]
        jc      ndl_nomem
        mov     [nd_blk],eax
        mov     dword ptr [eax-MCB_LEN+MCB_OWNER],0      ; the kernel's

        ; the image and its table, from where the stub ends
        mov     esi,[nd_sft]
        mov     eax,[nd_skip]
        mov     [esi+SFT_POS],eax
        mov     edi,[nd_blk]
        mov     ecx,[nd_imglen]
        call    file_read
        jc      ndl_readbad
        cmp     edx,[nd_imglen]
        jne     ndl_readbad
        mov     esi,[nd_sft]
        call    sft_close
        mov     dword ptr [nd_sft],0

        ; THE RELOCATION.  Every entry names a dword that holds an
        ; address as the linker wrote it, and the difference between
        ; where the image was linked and where it is goes into each.
        ; An entry has to name a dword that is whole, inside the image
        ; and before the table.
        mov     esi,[nd_blk]
        mov     eax,esi
        sub     eax,[esi+D32H_BASE]     ; EAX = the delta
        mov     edi,[nd_reloff]
        add     edi,esi                 ; EDI -> the table
        mov     ecx,[nd_nreloc]
        jecxz   ndl_relocated
ndl_reloc:
        mov     ebx,[edi]
        add     ebx,4
        jc      ndl_badimage
        cmp     ebx,[nd_reloff]
        ja      ndl_badimage
        add     [esi+ebx-4],eax
        add     edi,4
        dec     ecx
        jnz     ndl_reloc
ndl_relocated:
        cmp     [esi+D32H_BASE],esi     ; the header's own address was in
        jne     ndl_badimage            ; the table, or this is not an image

        ; the BSS, where the table was and beyond it
        mov     edi,esi
        add     edi,[nd_keep]
        mov     ecx,[nd_total]
        sub     ecx,[nd_keep]
        xor     eax,eax
        cld
        rep     stosb

        ; THE DRIVER TABLE: inside the image, with the signature and
        ; the version this kernel speaks.
        mov     ebx,[esi+D32H_ENTRY]    ; absolute now
        mov     eax,ebx
        sub     eax,esi
        cmp     eax,D32H_SIZE
        jb      ndl_badimage
        mov     ecx,eax                 ; where the table is in the image
        add     eax,NDT_LEN
        jc      ndl_badimage
        cmp     eax,[nd_reloff]         ; the image ends where the
        ja      ndl_badimage            ; relocation table starts
        cmp     dword ptr [ebx+NDT_SIG],NDT_SIGVAL
        jne     ndl_badimage
        cmp     word ptr [ebx+NDT_VER],NDT_VERSION
        je      ndl_tabv1

        ; A DISK DRIVER'S TABLE is the other version and the longer one,
        ; and the version and the flag have to agree: the fields after
        ; NDT_CALL are read once INIT is over, and a table that does not
        ; have them would be read past its end.
        cmp     word ptr [ebx+NDT_VER],NDT_DISKVER
        jne     ndl_badimage
        test    word ptr [ebx+NDT_FLAGS],NDTF_DISK
        jz      ndl_badimage
        add     ecx,NDT_DLEN
        jc      ndl_badimage
        cmp     ecx,[nd_reloff]
        ja      ndl_badimage
        jmp     short ndl_tabok
ndl_tabv1:
        test    word ptr [ebx+NDT_FLAGS],NDTF_DISK
        jnz     ndl_badimage
ndl_tabok:

        ; THE ENTRY POINTS, inside the image as the table is.  INIT has
        ; to be there; PARK, HALT and CALL are there or are 0.
        mov     eax,[ebx+NDT_INIT]
        sub     eax,esi
        cmp     eax,[nd_reloff]
        jae     ndl_badimage
        mov     ecx,NDT_PARK
ndl_entry:
        mov     eax,[ebx+ecx]
        or      eax,eax
        jz      ndl_entnext
        sub     eax,esi
        cmp     eax,[nd_reloff]
        jae     ndl_badimage
ndl_entnext:
        add     ecx,4
        cmp     ecx,NDT_CALL
        jbe     ndl_entry

        ; a row, and the block trimmed to what stays
        cmp     dword ptr [nd_count],ND_MAX
        jae     ndl_full
        mov     edi,[nd_count]
        imul    edi,ND_LEN
        add     edi,OFFSET nd_tab
        push    edi
        push    esi
        lea     esi,[ebx+NDT_NAME]
        mov     ecx,8
        rep     movsb
        pop     esi
        pop     edi
        mov     [edi+ND_BASE],esi
        mov     eax,[nd_total]
        mov     [edi+ND_SIZE],eax
        mov     [edi+ND_TABLE],ebx
        movzx   eax,word ptr [ebx+NDT_FLAGS]
        mov     [edi+ND_FLAGS],eax
        mov     dword ptr [edi+ND_PARKS],0
        mov     dword ptr [edi+ND_UNPARKS],0
        ; NOTHING ALLOCATED YET.  The counts go with the row, and a
        ; driver refused in this row before may have left some behind.
        mov     eax,[nd_count]
        mov     dword ptr [nd_mem+eax*NM_LEN+NM_BYTES],0
        mov     dword ptr [nd_mem+eax*NM_LEN+NM_BLOCKS],0
        mov     edx,esi
        mov     ebx,[nd_total]
        call    arena_resize            ; smaller or the same: a refusal
                                        ; leaves a block that is merely
                                        ; too big, which is harmless

        ; INIT.  The row is filled in first so that the driver can
        ; read where it is; the count is raised afterwards so that a
        ; driver that refuses was never there.  ECX and EBX are the
        ; letters a disk driver's units may have (dl_unitroom); every
        ; driver is told, and only a disk driver reads them.
        mov     ebx,[edi+ND_TABLE]
        mov     eax,[ebx+NDT_INIT]
        mov     [nd_callee],eax
        call    dl_unitroom             ; -> ECX letters left, EBX the next
        call    dl_freemask             ; -> EBP the letters free, by bit
        mov     esi,OFFSET nds_table
        mov     edx,OFFSET dl_line
        cld
        mov     byte ptr [nd_ininit],1  ; nd_whose looks at this row too
        call    dword ptr [nd_callee]
        mov     byte ptr [nd_ininit],0  ; MOV leaves CF alone
        jc      ndl_refused
        mov     esi,[nd_count]          ; the row again: INIT may have used
        imul    esi,ND_LEN              ; every register
        add     esi,OFFSET nd_tab
        inc     dword ptr [nd_count]

        ; A DISK DRIVER'S UNITS, now that it has said what they are.  It
        ; stays loaded if none of them could have a letter: INIT is over
        ; and may have hooked things a refusal would have undone, so the
        ; one safe answer is the message.
        test    dword ptr [esi+ND_FLAGS],NDTF_DISK
        jz      ndl_up
        call    dl_ndunits              ; ESI -> the row
        jc      ndl_nounits
ndl_up:
        popad
        clc
        ret
ndl_nounits:
        mov     eax,DLERR_NOUNITS
        jmp     ndl_fail

ndl_refused:
        mov     edx,[nd_blk]
        call    arena_free
        mov     eax,DLERR_INIT
        jmp     ndl_fail
ndl_full:
        mov     edx,[nd_blk]
        call    arena_free
        mov     eax,DLERR_FULL
        jmp     ndl_fail
ndl_badimage:
        mov     edx,[nd_blk]
        call    arena_free
        mov     eax,DLERR_NOFILE
        jmp     ndl_fail
ndl_readbad:
        mov     edx,[nd_blk]
        call    arena_free
        mov     eax,DLERR_NOFILE
        jmp     ndl_close
ndl_nomem:
        mov     eax,DLERR_NOMEM
        jmp     ndl_close
ndl_badfile:
        mov     eax,DLERR_NOFILE
ndl_close:
        mov     [nd_err],eax
        mov     esi,[nd_sft]
        call    sft_close
        mov     eax,[nd_err]
        jmp     ndl_fail
ndl_closenot:
        mov     esi,[nd_sft]
        call    sft_close
ndl_notours:
        xor     eax,eax
ndl_fail:
        mov     [nd_err],eax
        popad
        mov     eax,[nd_err]
        stc
        ret

;=====================================================================
; nd_park - AL = 0 before a park, 1 after it (PARK.INC).  Every driver
;           with a PARK entry, the last loaded first on the way down
;           and the first loaded first on the way back, as drivers
;           have always been unwound.  Everything preserved.
;=====================================================================
nd_park:
        pushad
        mov     ecx,[nd_count]
        jecxz   ndp_out
        or      al,al
        jnz     ndp_up
        imul    esi,ecx,ND_LEN
        add     esi,OFFSET nd_tab - ND_LEN
ndp_down:
        inc     dword ptr [esi+ND_PARKS]
        mov     ebx,[esi+ND_TABLE]
        mov     ebx,[ebx+NDT_PARK]
        or      ebx,ebx
        jz      ndp_dnext
        pushad
        cld
        call    ebx
        popad
ndp_dnext:
        sub     esi,ND_LEN
        dec     ecx
        jnz     ndp_down
        jmp     short ndp_out
ndp_up:
        mov     esi,OFFSET nd_tab
ndp_upnext:
        inc     dword ptr [esi+ND_UNPARKS]
        mov     ebx,[esi+ND_TABLE]
        mov     ebx,[ebx+NDT_PARK]
        or      ebx,ebx
        jz      ndp_unext
        pushad
        cld
        call    ebx
        popad
ndp_unext:
        add     esi,ND_LEN
        dec     ecx
        jnz     ndp_upnext
ndp_out:
        popad
        ret

;=====================================================================
; nd_halt - the machine is about to restart through the ROM or switch
;           off: every driver's HALT entry, last loaded first, once.
;           Called by fF1_reboot and fF1_power here, and by PMIO's
;           sys_reboot through GV_HALTHOOK, which is why it is once -
;           a restart asked for through 21h/F1h passes both.
;           Everything preserved; interrupts may be off.
;=====================================================================
nd_halt:
        pushad
        cmp     byte ptr [nd_halted],0
        jne     ndh_out
        mov     byte ptr [nd_halted],1
        mov     ecx,[nd_count]
        jecxz   ndh_out
        imul    esi,ecx,ND_LEN
        add     esi,OFFSET nd_tab - ND_LEN
ndh_next:
        mov     ebx,[esi+ND_TABLE]
        mov     ebx,[ebx+NDT_HALT]
        or      ebx,ebx
        jz      ndh_skip
        pushad
        cld
        call    ebx
        popad
ndh_skip:
        sub     esi,ND_LEN
        dec     ecx
        jnz     ndh_next
ndh_out:
        popad
        ret

;=====================================================================
; nd_find_flag - EAX = an NDTF_* flag (one bit), EDX = a 0-based index:
;                ESI -> the index-th loaded driver whose ND_FLAGS has
;                that flag, CF if there is no such row.  Preserves every
;                register but ESI.
;=====================================================================
nd_find_flag:
        push    ecx
        push    edx
        mov     ecx,[nd_count]
        mov     esi,OFFSET nd_tab
nff_next:
        jecxz   nff_none
        test    dword ptr [esi+ND_FLAGS],eax
        jz      nff_skip
        or      edx,edx
        jz      nff_found
        dec     edx
nff_skip:
        add     esi,ND_LEN
        dec     ecx
        jmp     nff_next
nff_found:
        pop     edx
        pop     ecx
        clc
        ret
nff_none:
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; nd_call_row - ESI -> a driver's row, EBX = the function, ECX = the
;               buffer's length, EDI -> the buffer: call that driver's
;               CALL entry (the contract NDRV.INC gives it, ESI -> the
;               row) -> EAX = what it answered, or CF and AX = ERR_FUNC
;               for a driver with no CALL entry.
;=====================================================================
nd_call_row:
        mov     eax,[esi+ND_TABLE]
        mov     eax,[eax+NDT_CALL]
        or      eax,eax
        jz      ncr_nofn
        cld
        jmp     eax                     ; the entry returns to our caller
ncr_nofn:
        mov     ax,ERR_FUNC
        stc
        ret

;=====================================================================
; brg_entry - the one routine the Windows-for-Workgroups bridge VxD
;             (PMDOS.386, step 7) calls, through a CR3 switch into this
;             PARKED kernel's own page directory, to reach a loaded
;             32-bit driver.  By then WIN386 owns the machine; this runs
;             with the kernel's page tables active (so kernel code and
;             nd_tab are mapped), DS and ES flat, interrupts off, on a
;             scratch stack the VxD set up in low 1:1 memory.  It does
;             exactly what ndsv_niccall / fF1_sock do - find the driver
;             with the flag and call it - and nothing that waits, which
;             is the rule every 32-bit driver already keeps.
;
;   In:  EAX = an NDTF_* flag (one bit), EDX = a 0-based index among the
;             loaded drivers with that flag, EBX = the driver's function,
;             ECX = the buffer length, EDI = the buffer (a linear address
;             the VxD mapped so both the kernel and WIN386 see it 1:1)
;   Out: EAX = what the driver answered; CF and AX = ERR_BADDRIVE when
;             there is no such driver, or the driver's own error.
;=====================================================================
brg_entry:
        cmp     eax,NDTF_BLOCK          ; the block sentinel is not a driver -
        je      brg_block               ; it goes straight to the block layer
        call    nd_find_flag            ; EAX = flag, EDX = index -> ESI = row
        jc      brge_nodrv
IFDEF BRG_FINDONLY
        mov     eax,esi                 ; bisection: prove find works, do not
        clc                             ; call the driver
        ret
ENDIF
IFDEF BRG_READCALL
        mov     ebx,[esi+ND_TABLE]      ; bisection: read (not execute) the
        mov     ebx,[ebx+NDT_CALL]      ; driver's CALL entry - its first
        mov     eax,[ebx]               ; dword should be 0C7FC071Eh (push ds,
        clc                             ; pop es, cld, mov...): proves the
        ret                             ; arena code survives and is mapped
ENDIF
        call    nd_call_row             ; ESI = row, EBX/ECX/EDI set -> EAX
        ret
brg_block:
        ; EDI = the block_req buffer (a linear 1:1 address the VxD mapped);
        ; blk_bridge reads/writes the USB sectors and answers in the block.
        call    dword ptr ds:[IOS_BRGBLK]
        ret
brge_nodrv:
        mov     ax,ERR_BADDRIVE
        stc
        ret

;=====================================================================
; fF1_ndrv - 21h/F1h AL=25h: the 32-bit drivers, for a program.
;            Bits 16-23 of EAX = an NDF_* function (INC\NDRV.INC).
;            Native programs only: EDI is a flat pointer, and a box's
;            is not.
;=====================================================================
fF1_ndrv:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     i21_unsup
        movzx   eax,byte ptr [ebp+F_EAX+2]
        cmp     eax,NDF_COUNT
        je      fnd_count
        cmp     eax,NDF_INFO
        je      fnd_info
        cmp     eax,NDF_CALL
        je      fnd_call
        cmp     eax,NDF_MEM
        je      fnd_mem
        jmp     i21_unsup

fnd_count:
        mov     eax,[nd_count]
        mov     [ebp+F_ECX],eax
        mov     dword ptr [ebp+F_EAX],ND_MAX
        jmp     i21_ok

fnd_info:
        movzx   edx,byte ptr [ebp+F_EDX]
        cmp     edx,[nd_count]
        jae     fnd_norow
        mov     eax,[ebp+F_EDI]
        mov     ecx,ND_LEN
        call    cbuf_ok
        jc      buf_deny
        imul    esi,edx,ND_LEN
        add     esi,OFFSET nd_tab
        mov     edi,[ebp+F_EDI]
        mov     ecx,ND_LEN / 4
        cld
        rep     movsd
        jmp     i21_ok

fnd_call:
        movzx   edx,byte ptr [ebp+F_EDX]
        cmp     edx,[nd_count]
        jae     fnd_norow
        mov     ecx,[ebp+F_ECX]
        or      ecx,ecx
        jz      fnd_nobuf
        mov     eax,[ebp+F_EDI]
        call    cbuf_ok
        jc      buf_deny
fnd_nobuf:
        imul    esi,edx,ND_LEN
        add     esi,OFFSET nd_tab
        mov     ebx,[ebp+F_EBX]
        mov     ecx,[ebp+F_ECX]
        mov     edi,[ebp+F_EDI]
        call    nd_call_row             ; ESI -> the row, as promised
        jc      fnd_said_no
        mov     [ebp+F_EAX],eax
        jmp     i21_ok
fnd_said_no:
        jmp     i21_fail                ; AX = the driver's error

; NDF_MEM: what the row's driver holds through NDS_ALLOC, as ndsv_alloc
; and ndsv_free counted it.  The image itself is ND_SIZE in the row.
fnd_mem:
        movzx   edx,byte ptr [ebp+F_EDX]
        cmp     edx,[nd_count]
        jae     fnd_norow
        mov     eax,dword ptr [nd_mem+edx*NM_LEN+NM_BYTES]
        mov     [ebp+F_ECX],eax
        mov     eax,dword ptr [nd_mem+edx*NM_LEN+NM_BLOCKS]
        mov     [ebp+F_EDX],eax
        jmp     i21_ok
fnd_norow:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;=====================================================================
; fF1_sock - 21h/F1h AL=26h: the SOCKETS, for a program.  Bits 16-23
;            of EAX = a socket function (INC\SOCKET.H), EDI -> a request
;            buffer of ECX bytes (0: none), which the kernel has checked.
;            The call goes to the one loaded driver that is the TCP/IP
;            stack (NDTF_STACK), whose CALL answers EAX, or CF and an
;            error.  No stack loaded: CF and AX = ERR_GENERAL.  Native
;            programs only: EDI is a flat pointer, and a box's is not.
;=====================================================================
fF1_sock:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     i21_unsup
        mov     eax,NDTF_STACK
        xor     edx,edx
        call    nd_find_flag            ; ESI -> the stack's row
        jc      fsk_nostack
        mov     ecx,[ebp+F_ECX]
        or      ecx,ecx
        jz      fsk_nobuf
        mov     eax,[ebp+F_EDI]
        call    cbuf_ok
        jc      buf_deny
fsk_nobuf:
        mov     eax,[ebp+F_EAX]
        shr     eax,16
        mov     ebx,eax                 ; the socket function
        mov     ecx,[ebp+F_ECX]
        mov     edi,[ebp+F_EDI]
        call    nd_call_row             ; ESI -> the stack's row
        jc      fsk_said_no
        mov     [ebp+F_EAX],eax
        jmp     i21_ok
fsk_said_no:
        jmp     i21_fail
fsk_nostack:
        mov     ax,ERR_GENERAL
        jmp     i21_fail

;=====================================================================
; THE SERVICES a driver is given (NDS_*, INC\NDRV.INC).  Each is a
; routine with the register contract of the kernel routine behind it;
; the PMIO ones are reached through IOSVC, which is why a driver is
; handed addresses and not the table's layout.
;=====================================================================
nds_table:
        dd      (NDS_LEN SHL 16) OR NDS_VERSION ; NDS_VER
        dd      OFFSET ndsv_alloc                ; NDS_ALLOC
        dd      OFFSET ndsv_free                 ; NDS_FREE
        dd      OFFSET ndsv_print                ; NDS_PRINT
        dd      OFFSET ndsv_ticks                ; NDS_TICKS
        dd      OFFSET ndsv_bus                  ; NDS_BUS
        dd      OFFSET ndsv_irqhook              ; NDS_IRQHOOK
        dd      OFFSET ndsv_irqunhook            ; NDS_IRQUNHOOK
        dd      OFFSET ndsv_tickhook             ; NDS_TICKHOOK
        dd      OFFSET ndsv_tickunhook           ; NDS_TICKUNHOOK
        dd      OFFSET ndsv_niccount             ; NDS_NICCOUNT
        dd      OFFSET ndsv_niccall              ; NDS_NICCALL
        dd      OFFSET ndsv_sockcall             ; NDS_SOCKCALL
        dd      OFFSET ndsv_stamp                ; NDS_STAMP
        dd      OFFSET ndsv_flagcall             ; NDS_FLAGCALL
IF ($ - nds_table) NE NDS_LEN
        .ERR    <nds_table does not match NDS_LEN in NDRV.INC>
ENDIF

;--- ndsv_alloc - EBX = bytes -> EAX, CF.  First fit and the kernel's,
;--- whatever the running program's strategy is and whoever it is: a
;--- driver's memory outlives every program.  The block goes on the
;--- count of the driver whose code made the call (nd_whose), which is
;--- what NDF_MEM reads back for MEM /D.
ndsv_alloc:
        push    ebx
        mov     ebx,[alloc_strat]
        push    ebx
        mov     dword ptr [alloc_strat],AF_FIRST
        mov     ebx,[esp+4]
        call    arena_alloc
        pop     dword ptr [alloc_strat]
        pop     ebx
        jc      nda_out
        mov     dword ptr [eax-MCB_LEN+MCB_OWNER],0
        push    edx
        push    esi
        mov     edx,[esp+8]             ; where the call came from
        call    nd_whose                ; -> ESI = that driver's counts
        jc      nda_nobody
        mov     edx,[eax-MCB_LEN+MCB_SIZE]
        add     [esi+NM_BYTES],edx
        inc     dword ptr [esi+NM_BLOCKS]
nda_nobody:
        pop     esi
        pop     edx
        clc
nda_out:
        ret

;--- ndsv_free - EDX = the block, CF if it is not one.  It comes off the
;--- count of the driver whose code made the call, as ndsv_alloc put it
;--- on.  A count that would go below nothing is left as it is: that
;--- driver is freeing a block it was never given.
ndsv_free:
        push    ecx
        push    esi
        mov     ecx,[edx-MCB_LEN+MCB_SIZE]
        call    arena_free
        jc      nfr_out
        push    edx
        mov     edx,[esp+12]            ; where the call came from
        call    nd_whose                ; -> ESI = that driver's counts
        pop     edx
        jc      nfr_done
        cmp     dword ptr [esi+NM_BLOCKS],0
        je      nfr_done
        cmp     [esi+NM_BYTES],ecx
        jb      nfr_done
        sub     [esi+NM_BYTES],ecx
        dec     dword ptr [esi+NM_BLOCKS]
nfr_done:
        clc
nfr_out:
        pop     esi
        pop     ecx
        ret

;--- nd_whose - EDX = an address in some driver's code -> ESI -> the
;--- counts (NM_*) of the driver whose image holds it, CF if none does.
;--- A driver's calls to NDS_ALLOC and NDS_FREE come from its own image,
;--- the C ones through NDSTART.ASM, which is linked into it; so the
;--- return address says whose block it is and the services need no
;--- word from the driver.  During INIT the row being loaded is
;--- searched as well, since nd_count does not include it until INIT
;--- has said yes.  Preserves every register but ESI.
nd_whose:
        push    eax
        push    ecx
        push    edi
        mov     ecx,[nd_count]
        movzx   eax,byte ptr [nd_ininit]
        add     ecx,eax
        mov     edi,OFFSET nd_tab
        mov     esi,OFFSET nd_mem
ndw_next:
        jecxz   ndw_none
        mov     eax,edx
        sub     eax,[edi+ND_BASE]
        cmp     eax,[edi+ND_SIZE]
        jb      ndw_found               ; below the base wraps, and fails
        add     edi,ND_LEN
        add     esi,NM_LEN
        dec     ecx
        jmp     ndw_next
ndw_found:
        pop     edi
        pop     ecx
        pop     eax
        clc
        ret
ndw_none:
        pop     edi
        pop     ecx
        pop     eax
        stc
        ret

ndsv_print:
        jmp     dword ptr ds:[IOS_CONSTR]
ndsv_ticks:
        jmp     dword ptr ds:[IOS_TICKS]
ndsv_bus:
        jmp     dword ptr ds:[IOS_BUS]
ndsv_irqhook:
        jmp     dword ptr ds:[IOS_IRQHOOK]
ndsv_irqunhook:
        jmp     dword ptr ds:[IOS_IRQUNHOOK]
ndsv_tickhook:
        jmp     dword ptr ds:[IOS_TICKHOOK]
ndsv_tickunhook:
        jmp     dword ptr ds:[IOS_TICKUNHOOK]

;--- ndsv_niccount - -> EAX = loaded drivers that are NICs (NDTF_NIC)
ndsv_niccount:
        push    ecx
        push    esi
        xor     eax,eax
        mov     ecx,[nd_count]
        jecxz   nvc_out
        mov     esi,OFFSET nd_tab
nvc_next:
        test    dword ptr [esi+ND_FLAGS],NDTF_NIC
        jz      nvc_skip
        inc     eax
nvc_skip:
        add     esi,ND_LEN
        dec     ecx
        jnz     nvc_next
nvc_out:
        pop     esi
        pop     ecx
        ret

;--- ndsv_niccall - EAX = a NIC index, EBX = a NIC_* function, EDI ->
;--- the buffer, ECX = its length -> EAX = what the card answered, or
;--- CF and AX = ERR_* (the stack rides on the card drivers through
;--- here, as a program does through NDF_CALL).  The buffer is the
;--- caller's own kernel memory, so it is not checked the way a
;--- program's EDI is.
ndsv_niccall:
        mov     edx,eax                         ; the index
        mov     eax,NDTF_NIC
        call    nd_find_flag                    ; ESI -> that NIC's row
        jc      nvl_norow
        jmp     nd_call_row                     ; EBX, ECX, EDI already set
nvl_norow:
        mov     ax,ERR_BADDRIVE
        stc
        ret

;--- ndsv_sockcall - EBX = a socket function (INC\SOCKET.H), EDI -> the
;--- request, ECX = its length -> EAX = what the one NDTF_STACK driver
;--- answered, or CF and AX = ERR_GENERAL with no stack loaded.  What
;--- fF1_sock does for a program, for a redirector that is itself a
;--- driver; the buffer is the caller's kernel memory, not checked.
ndsv_sockcall:
        push    ebx
        mov     eax,NDTF_STACK
        xor     edx,edx
        call    nd_find_flag                    ; ESI -> the stack's row
        pop     ebx
        jc      nscl_nostack
        jmp     nd_call_row                     ; EBX, ECX, EDI already set
nscl_nostack:
        mov     ax,ERR_GENERAL
        stc
        ret

;--- ndsv_flagcall - EAX = an NDTF_* flag, EBX = a function, EDI -> the
;--- request, ECX = its length -> EAX = what the first loaded driver with
;--- that flag answered, or CF and AX = ERR_GENERAL when none has it.
;--- ndsv_sockcall for any flag; the buffer is the caller's kernel memory.
ndsv_flagcall:
        push    ebx
        xor     edx,edx
        call    nd_find_flag                    ; ESI -> the driver's row
        pop     ebx
        jc      nfcl_none
        jmp     nd_call_row                     ; EBX, ECX, EDI already set
nfcl_none:
        mov     ax,ERR_GENERAL
        stc
        ret

;--- ndsv_stamp - -> EAX = now, the date in the high word and the time
;--- in the low, DOS-packed: what a directory entry would be stamped
;--- with (INT21.INC's get_dosdate and get_dostime, which ask the BIOS)
ndsv_stamp:
        push    edx
        call    get_dostime
        movzx   edx,ax
        call    get_dosdate
        shl     eax,16
        or      eax,edx
        pop     edx
        ret

;=====================================================================
; Data
;=====================================================================
nd_stubname db  'PMD32DRV'              ; DRVSTUB_NAME: the mark in a
                                        ; real-mode header
nd_count    dd  0                       ; rows of nd_tab in use
nd_halted   db  0                       ; nd_halt has run
nd_ininit   db  0                       ; a driver's INIT is running, in
                                        ; row nd_count
            db  0, 0
nd_tab      db  ND_MAX * ND_LEN dup(0)  ; the drivers, in loading order

nd_mem      db  ND_MAX * NM_LEN dup(0)  ; what each row's driver holds
                                        ; through NDS_ALLOC: NM_* above

nd_sft      dd  0                       ; the file, while it is open
nd_fsize    dd  0
nd_got      dd  0                       ; bytes the probe read
nd_skip     dd  0                       ; where the image starts in it
nd_imglen   dd  0                       ; the image and its table
nd_reloff   dd  0                       ; the table, from the image's start
nd_nreloc   dd  0
nd_keep     dd  0                       ; the image, on a paragraph
nd_total    dd  0                       ; ...and the BSS: what stays
nd_blk     dd  0                       ; the block
nd_err      dd  0
nd_callee   dd  0                       ; an entry, while it runs

; The probe buffer is read into for every DEVICE= line and holds
; nothing between them: below the arena, and not in the file.
_DOS    ENDS
_KBSS   SEGMENT
nd_probe    db  ND_PROBELEN dup(?)
_KBSS   ENDS
_DOS    SEGMENT
