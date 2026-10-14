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
        add     eax,NDT_LEN
        jc      ndl_badimage
        cmp     eax,[nd_keep]
        ja      ndl_badimage
        cmp     dword ptr [ebx+NDT_SIG],NDT_SIGVAL
        jne     ndl_badimage
        cmp     word ptr [ebx+NDT_VER],NDT_VERSION
        jne     ndl_badimage

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
        mov     dword ptr [edi+ND_FLAGS],0
        mov     dword ptr [edi+ND_PARKS],0
        mov     dword ptr [edi+ND_UNPARKS],0
        mov     edx,esi
        mov     ebx,[nd_total]
        call    arena_resize            ; smaller or the same: a refusal
                                        ; leaves a block that is merely
                                        ; too big, which is harmless

        ; INIT.  The row is filled in first so that the driver can
        ; read where it is; the count is raised afterwards so that a
        ; driver that refuses was never there.
        mov     ebx,[edi+ND_TABLE]
        mov     esi,OFFSET nds_table
        mov     edx,OFFSET dl_line
        cld
        call    dword ptr [ebx+NDT_INIT]
        jc      ndl_refused
        inc     dword ptr [nd_count]
        popad
        clc
        ret

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
        imul    esi,edx,ND_LEN
        add     esi,OFFSET nd_tab
        mov     ebx,[esi+ND_TABLE]
        mov     ebx,[ebx+NDT_CALL]
        or      ebx,ebx
        jz      i21_unsup
        mov     [nd_callee],ebx
        mov     ecx,[ebp+F_ECX]
        or      ecx,ecx
        jz      fnd_nobuf
        mov     eax,[ebp+F_EDI]
        call    cbuf_ok
        jc      buf_deny
fnd_nobuf:
        mov     ebx,[ebp+F_EBX]
        mov     ecx,[ebp+F_ECX]
        mov     edx,[ebp+F_EDX]
        mov     edi,[ebp+F_EDI]
        cld
        call    dword ptr [nd_callee]   ; ESI -> the row, as promised
        jc      fnd_said_no
        mov     [ebp+F_EAX],eax
        jmp     i21_ok
fnd_said_no:
        jmp     i21_fail                ; AX = the driver's error
fnd_norow:
        mov     ax,ERR_BADDRIVE
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
IF ($ - nds_table) NE NDS_LEN
        .ERR    <nds_table does not match NDS_LEN in NDRV.INC>
ENDIF

;--- ndsv_alloc - EBX = bytes -> EAX, CF.  First fit and the kernel's,
;--- whatever the running program's strategy is and whoever it is: a
;--- driver's memory outlives every program.
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
        clc
nda_out:
        ret

;--- ndsv_free - EDX = the block
ndsv_free:
        jmp     arena_free

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

;=====================================================================
; Data
;=====================================================================
nd_stubname db  'PMD32DRV'              ; DRVSTUB_NAME: the mark in a
                                        ; real-mode header
nd_count    dd  0                       ; rows of nd_tab in use
nd_halted   db  0                       ; nd_halt has run
            db  0, 0, 0
nd_tab      db  ND_MAX * ND_LEN dup(0)  ; the drivers, in loading order

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
nd_callee   dd  0                       ; a CALL entry, while it runs

; The probe buffer is read into for every DEVICE= line and holds
; nothing between them: below the arena, and not in the file.
_DOS    ENDS
_KBSS   SEGMENT
nd_probe    db  ND_PROBELEN dup(?)
_KBSS   ENDS
_DOS    SEGMENT
