;=====================================================================
; DEVLOAD.INC - DEVICE= and DEVICEHIGH=: loading a real DOS driver
;
; PM-DOS's own four devices are 32-bit code inside the kernel, called
; as subroutines.  A driver named on a DEVICE= line is none of those
; things: it is a lump of 16-bit real-mode code that expects to be
; loaded at a paragraph boundary, spoken to through two far entries
; and a request packet, and left resident afterwards.  Everything here
; exists to make that possible from a kernel that is not in real mode
; and has no intention of going back.
;
; THE FIVE PIECES, and each was finishable on its own:
;
;   1  read the .SYS into conventional memory.  One file may hold
;      SEVERAL chained headers, so the chain inside the file is walked
;      rather than assumed to be one deep.
;   2  call INIT through strategy and interrupt with a packet.  This
;      is the only genuinely new machinery: an EXCURSION into V86 from
;      32-bit code, with no program, no PSP and no DOS box around it.
;   3  keep what INIT says to keep, hand the rest back, and link the
;      header behind NUL - which is why NUL is first in the chain and
;      always was.
;   4  make dev_find look HERE FIRST, so a loaded CON beats the
;      kernel's own.  That is the entire point of loading one, and the
;      other order would make ANSI.SYS load perfectly and do nothing.
;   5  route dev_in and dev_out through packets for anything loaded.
;
; WHAT IS DELIBERATELY NOT HERE.  Block drivers: their units have to
; become drive letters in a volume table that gets its letters by
; scanning partitions, which is a different piece of work and wants
; its own release.  A DEVICE= line naming one is refused with a
; message rather than half-obeyed.
;
; Included by DOSMAIN.ASM after VDM.INC: the excursion services the
; driver's own INT 21h calls with vdm_do21, which is the same routine
; a DOS box's calls go through.
;=====================================================================

DL_MAX          EQU     16              ; loaded device NAMES, not files
DLE_NAME        EQU     0               ; 8 bytes, blank padded
DLE_SEG         EQU     8               ; dw - the header's segment
DLE_HDR         EQU     10              ; dw - and its offset in it
DLE_LEN         EQU     12

; Block drivers.  A unit is a DRIVE LETTER, and the table below is what
; turns the pseudo BIOS drive number in drv_tbl back into "unit U of
; the driver in dl_tab row T".
DL_UNIT_BASE    EQU     0E0h            ; pseudo BIOS drive numbers for them
DLU_MAX         EQU     16              ; driver units, in total
DLU_TAB         EQU     0               ; dd - which dl_tab row
DLU_UNIT        EQU     4               ; db - which unit of that driver
DLU_MEDIA       EQU     5               ; db - its media descriptor
DLU_LEN         EQU     8
DL_IOSECS       EQU     16              ; the staging buffer, in sectors

DL_PATH_MAX     EQU     80

; The scratch block, and everything in it is there because a real-mode
; driver has to be able to reach it with a segment and an offset:
;
;   0000  the return pad - one HLT
;   0010  the INIT command line, where the packet's ARG pointer aims
;   0090  level 0: packet (20h), then 100h of stack
;   01B0  level 1, and so on
;
; NESTING IS REAL AND IS NOT A CORNER CASE.  A loaded CON driver -
; which is what ANSI.SYS is - means every character the kernel prints
; goes through a driver call, so a driver's INIT printing its own
; banner is a driver call made from inside a driver call.  ANSI.SYS
; prints a banner.  Four levels is three more than that needs and
; still costs only 1.2K.
DL_LEVELS       EQU     4
DL_PAD          EQU     0000h           ; where the HLT lives
DL_ARG          EQU     0010h
DL_ARG_MAX      EQU     0080h
DL_BASE         EQU     0090h
DL_PKTLEN       EQU     0020h
DL_STRIDE       EQU     0120h           ; the packet and its stack
DL_STK          EQU     0120h           ; stack top, measured from the packet
DL_SIZE         EQU     DL_BASE + DL_LEVELS * DL_STRIDE
DL_PARAS        EQU     (DL_SIZE + 15) / 16

DLERR_NOFILE    EQU     1
DLERR_NOMEM     EQU     2
DLERR_BLOCK     EQU     3               ; kept: the number is in no message
                                        ; now, and renumbering the rest to
                                        ; close the gap would be churn
DLERR_INIT      EQU     4
DLERR_FULL      EQU     5
DLERR_NOUNITS   EQU     6               ; a block driver with no units

;=====================================================================
; dl_init - the scratch block, taken from conventional memory the
;           first time a DEVICE= line is seen and never given back.
;
; Not allocated at boot, because a machine with no DEVICE= lines
; should not pay for one; not in the kernel's own data, because a
; real-mode driver cannot reach the kernel's own data.
;=====================================================================
dl_init:
        cmp     word ptr [dl_seg],0
        jne     dli_have
        pushad
        mov     ebx,DL_PARAS
        call    lm_alloc
        jc      dli_fail
        movzx   eax,ax
        mov     [dl_seg],eax
        mov     dx,8                    ; owned by DOS itself, so that the
        call    lm_setown               ; exit sweep never takes it back
        mov     edi,eax
        shl     edi,4
        mov     byte ptr [edi+DL_PAD],0F4h      ; the HLT a driver's RETF
        popad                                   ; comes home to
dli_have:
        clc
        ret
dli_fail:
        popad
        stc
        ret

;=====================================================================
; dl_enter / dl_leave - one level of driver call.
;
;   dl_enter -> EDI = flat address of this level's packet, zeroed
;               EBX = the same as an offset within dl_seg
;               CF if the nesting is deeper than DL_LEVELS
;
; The packet is zeroed because drivers read fields their command does
; not define often enough that leaving the last call's values there is
; asking for it.
;=====================================================================
dl_enter:
        push    eax
        push    ecx
        mov     eax,[dl_depth]
        cmp     eax,DL_LEVELS
        jae     dle_deep
        inc     dword ptr [dl_depth]
        imul    eax,DL_STRIDE
        add     eax,DL_BASE
        mov     ebx,eax
        movzx   edi,word ptr [dl_seg]
        shl     edi,4
        add     edi,eax
        push    edi
        xor     eax,eax
        mov     ecx,DL_PKTLEN / 4
        rep     stosd
        pop     edi
        pop     ecx
        pop     eax
        clc
        ret
dle_deep:
        pop     ecx
        pop     eax
        stc
        ret

dl_leave:
        dec     dword ptr [dl_depth]
        ret

;=====================================================================
; dl_excur - THE EXCURSION.  Run [dl_entry] in the driver's segment,
;            in V86 mode, until it comes back.
;
; A far RETURN is how a driver finishes, and in a 32-bit kernel there
; is nowhere for it to return TO - so it returns to a HLT the kernel
; planted, and the monitor stopping on that byte at that address is
; the signal.  Same trick the XMS and DPMI entries use, and for the
; same reason: a HLT is the one instruction whose only possible
; meaning here is "we are done".
;
; INT 21h IS TRAPPED; everything else is reflected to the real IVT,
; because a driver talking to its hardware through the BIOS is a
; driver doing its job.  A driver's INIT is entitled to make DOS calls
; - 09h to say what it is, 25h and 35h to hook a vector, 30h to check
; the version - and it makes them BEFORE there is a shell, a PSP or a
; program, which is a thing nothing else in this kernel has ever done.
;
; The live V86 context is parked on the kernel stack for the duration.
; A driver call can happen with a DOS box running and usually does:
; every TYPE of a device from inside a box arrives here.
;=====================================================================
dl_excur:
        pushad
        sub     esp,V86C_SIZE
        mov     esi,OFFSET vdm_ctx
        mov     edi,esp
        mov     ecx,V86C_SIZE / 4
        rep     movsd

        mov     edi,OFFSET vdm_ctx
        push    edi
        xor     eax,eax
        mov     ecx,V86C_SIZE / 4
        rep     stosd
        pop     edi

        mov     eax,[dl_seg]
        and     eax,0FFFFh
        mov     [edi+V86C_SS],eax
        mov     [edi+V86C_ES],eax
        mov     eax,[dl_drvseg]
        mov     [edi+V86C_CS],eax
        mov     [edi+V86C_DS],eax       ; a .SYS is one segment: CS is DS
        mov     eax,[dl_entry]
        mov     [edi+V86C_EIP],eax
        mov     eax,[dl_pktoff]
        mov     [edi+V86C_EBX],eax
        mov     dword ptr [edi+V86C_FLAGS],EFL_IF       ; V86_UFLAGS is a MASK, not
                                                ; a value: as a value it sets DF

        ; The far return address, on the driver's own stack.  RETF
        ; pops the offset first, so the pad's offset goes lower.
        mov     eax,[dl_stktop]
        sub     eax,4
        mov     [edi+V86C_ESP],eax
        mov     esi,[dl_seg]
        and     esi,0FFFFh
        shl     esi,4
        add     esi,eax
        mov     word ptr [esi],DL_PAD
        mov     eax,[dl_seg]
        mov     [esi+2],ax

        ; Vector 21h is bit 1 of byte 4: eight vectors to a byte, and
        ; 21h is 33.
        mov     byte ptr [edi+V86C_TRAP+4],02h

        ; AND 67h, WHEN THERE IS EXPANDED MEMORY, because a driver's
        ; INIT is entitled to ask for some: RAMDRIVE.SYS /A is exactly
        ; that driver, and it allocates its handle before it reports
        ; its unit count.  Bit 7 of byte 12.
        cmp     dword ptr [ems_have],0
        je      dlx_run
        mov     byte ptr [edi+V86C_TRAP+12],80h

dlx_run:
        mov     esi,OFFSET vdm_ctx
        call    dword ptr ds:[IOS_V86RUN]
        cmp     eax,V86R_HLT
        je      dlx_halt
        cmp     eax,V86R_INT
        jne     dlx_lost                ; a fault inside a driver is not
        cmp     ebx,67h                 ; ours to recover from
        je      dlx_ems
        cmp     ebx,21h
        jne     dlx_lost
        mov     al,byte ptr [vdm_ctx+V86C_EAX+1]
        cmp     al,4Ch                  ; a driver that TERMINATES has gone
        je      dlx_lost                ; wrong: there is no process here
        cmp     al,31h
        je      dlx_lost
        mov     al,byte ptr [vdm_ctx+V86C_EAX+1]
        call    vdm_do21
        jmp     short dlx_run

dlx_ems:
        call    ems_call
        jmp     short dlx_run

;--- the HLT.  WHICH one?  Only the pad ends the excursion; a HLT
;--- anywhere else is a driver that has stopped in the middle of
;--- itself, and carrying on would run whatever follows it.
dlx_halt:
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        mov     ebx,[dl_seg]
        cmp     eax,ebx
        jne     dlx_lost
        ; ...and the monitor stops ON the HLT, not past it - vf_hlt
        ; leaves EIP where it found it, which is what lets xms_iscb
        ; and the DPMI entries recognise theirs by the address they
        ; planted rather than by that address plus one.
        mov     ebx,DL_PAD
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        cmp     eax,ebx
        jne     dlx_lost
        jmp     short dlx_out
dlx_lost:
        mov     byte ptr [dl_lost],1
dlx_out:
        mov     esi,esp                 ; and the parked context back
        mov     edi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        rep     movsd
        add     esp,V86C_SIZE
        popad
        ret

;=====================================================================
; dl_go - strategy, then interrupt, on the driver at [dl_drvseg] with
;         its header at [dl_hdrflat].
;
; TWO ENTRIES FOR ONE OPERATION is an artefact of a 1981 plan for
; asynchronous drivers that never arrived.  Every driver ever shipped
; saves ES:BX in the first and does the work in the second, and a DOS
; that called only the second would break all of them - so both are
; called, in that order, exactly as they have been since 2.0.
;=====================================================================
dl_go:
        pushad
        mov     esi,[dl_hdrflat]
        movzx   eax,word ptr [esi+DEVH_STRAT]
        mov     [dl_entry],eax
        call    dl_excur
        cmp     byte ptr [dl_lost],0
        jne     dlg_out
        mov     esi,[dl_hdrflat]
        movzx   eax,word ptr [esi+DEVH_INTR]
        mov     [dl_entry],eax
        call    dl_excur
dlg_out:
        popad
        ret

;=====================================================================
; dl_prep / dl_fire - a driver call from outside.
;
;   dl_prep  EAX = the dl_tab index, ECX = the command
;            -> EDI = the packet, for the caller to fill in
;            CF if the index is bad or the nesting too deep
;   dl_fire  -> AX = the status word, CF on error.  Ends the level
;            either way, and leaves EDI still pointing at the packet:
;            the answer is IN the packet and the caller has to be able
;            to read it back.
;
; Two halves because every command has a different body, and the
; caller needs the address to fill it before the driver sees it.
;=====================================================================
dl_prep:
        cmp     eax,[dl_count]
        jae     dlp_bad
        push    ecx                     ; the command, which the row lookup
        push    eax                     ; below is about to want ECX for
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        movzx   ecx,word ptr [eax+DLE_SEG]
        mov     [dl_drvseg],ecx
        movzx   ecx,word ptr [eax+DLE_HDR]
        mov     [dl_hdroff],ecx
        mov     eax,[dl_drvseg]
        shl     eax,4
        add     eax,[dl_hdroff]
        mov     [dl_hdrflat],eax
        pop     eax
        pop     ecx
        call    dl_enter
        jc      dlp_bad
        mov     [dl_pktflat],edi
        mov     [dl_pktoff],ebx
        mov     eax,ebx
        add     eax,DL_STK
        mov     [dl_stktop],eax
        mov     byte ptr [edi+REQ_CMD],cl
        mov     byte ptr [edi+REQ_LEN],REQC_LEN
        clc
        ret
dlp_bad:
        stc
        ret

dl_fire:
        mov     byte ptr [dl_lost],0
        call    dl_go
        call    dl_leave
        mov     edi,[dl_pktflat]
        movzx   eax,word ptr [edi+REQ_STATUS]
        cmp     byte ptr [dl_lost],0
        jne     dlf_err
        test    eax,DEVS_ERROR
        jnz     dlf_err
        clc
        ret
dlf_err:
        stc
        ret

;=====================================================================
; dl_load - ESI -> the rest of the CONFIG.SYS line, which is a path
;           and then whatever the driver's own options are.
;           CF set with AX = DLERR_* if it did not load.
;=====================================================================
dl_load:
        pushad
        call    dl_init
        jc      dll_nomem

        ; The path is the first word of it; the WHOLE of it is what
        ; the driver is given, because that is what DOS passes and
        ; every driver with an option switch parses it for itself.
        call    dl_split
        jc      dll_nofile
        call    dl_setargs

        mov     esi,OFFSET dl_path
        xor     eax,eax                 ; read only, deny none
        call    open_core
        jc      dll_nofile
        mov     [dl_sft],esi

        mov     eax,[esi+SFT_SIZE]
        or      eax,eax
        jz      dll_nofile_c
        mov     [dl_fsize],eax
        add     eax,15
        shr     eax,4
        mov     ebx,eax
        call    lm_alloc
        jc      dll_nomem_c
        movzx   eax,ax
        mov     [dl_drvseg],eax
        mov     dx,8                    ; DOS owns it: a driver outlives
        call    lm_setown               ; every program in the machine

        mov     edi,[dl_drvseg]
        shl     edi,4
        mov     esi,[dl_sft]
        mov     ecx,[dl_fsize]
        call    file_read
        mov     [dl_got],edx
        mov     esi,[dl_sft]
        call    sft_close
        mov     edx,[dl_got]
        cmp     edx,[dl_fsize]
        jne     dll_initbad

        ; ...and now the headers inside it.  The chain is the file's
        ; own: each NEXT holds the offset of the one after it and
        ; FFFFh ends it.  One header is the normal case; two is not an
        ; error and is why this is a loop.
        mov     dword ptr [dl_hdroff],0
dll_hdr:
        mov     eax,[dl_drvseg]
        shl     eax,4
        add     eax,[dl_hdroff]
        mov     [dl_hdrflat],eax
        mov     esi,eax

        ; THE FILE'S OWN CHAIN, READ BEFORE THE REAL ONE IS TOUCHED.
        ; dl_chain writes this very field - it puts NUL's old next
        ; pointer in it - so asking afterwards where the file's next
        ; header is gets the answer "the console", and the loader walks
        ; off into the middle of the driver it just installed and tries
        ; to initialise whatever it finds there.
        mov     eax,[esi+DEVH_NEXT]
        mov     dword ptr [dl_fnext],eax

        ; CHARACTER OR BLOCK, and from here the two differ in exactly
        ; one place: a block driver's units have to become drive
        ; letters after it is chained, which is what dl_blkinit does.
        ; Everything before that - reading it in, calling INIT, keeping
        ; what INIT asked for, linking the header - is the same job.
        mov     ax,[esi+DEVH_ATTR]
        mov     byte ptr [dl_isblk],0
        test    ax,DEV_ATTR_CHAR
        jnz     dll_ischar
        mov     byte ptr [dl_isblk],1
dll_ischar:

        call    dl_doinit
        jc      dll_initbad
        call    dl_chain
        jc      dll_full

        cmp     byte ptr [dl_isblk],0
        je      dll_notblk
        mov     esi,[dl_pktflat]        ; INIT's answers are still in it
        call    dl_blkinit
        jc      dll_nounits
dll_notblk:

        mov     eax,dword ptr [dl_fnext]
        cmp     ax,0FFFFh
        je      dll_keep
        movzx   eax,ax
        mov     [dl_hdroff],eax
        jmp     short dll_hdr

;--- what INIT asked to keep, and not a paragraph more ---------------
dll_keep:
        mov     eax,[dl_endseg]
        shl     eax,4
        add     eax,[dl_endoff]
        mov     ecx,[dl_drvseg]
        shl     ecx,4
        sub     eax,ecx                 ; how many bytes of it are resident
        jbe     dll_initbad             ; at or below the start: it refused
        add     eax,15
        shr     eax,4
        mov     ebx,eax
        mov     eax,[dl_drvseg]
        call    lm_resize               ; smaller than we have, so it fits
        popad
        clc
        ret

dll_nofile_c:
        mov     esi,[dl_sft]
        call    sft_close
dll_nofile:
        mov     eax,DLERR_NOFILE
        jmp     short dll_fail
dll_nomem_c:
        mov     esi,[dl_sft]
        call    sft_close
dll_nomem:
        mov     eax,DLERR_NOMEM
        jmp     short dll_fail
dll_nounits:
        mov     eax,DLERR_NOUNITS
        jmp     short dll_undo
dll_initbad:
        mov     eax,DLERR_INIT
        jmp     short dll_undo
dll_full:
        mov     eax,DLERR_FULL
dll_undo:
        push    eax
        mov     eax,[dl_drvseg]
        call    lm_free
        pop     eax
dll_fail:
        mov     [dl_err],eax
        popad
        mov     eax,[dl_err]
        stc
        ret

;---------------------------------------------------------------------
; dl_split - ESI -> the line.  Copies the leading path into dl_path
;            and the whole line into dl_line.  CF if there is no path.
;---------------------------------------------------------------------
dl_split:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     edi,OFFSET dl_line      ; the whole of it, first
        xor     ecx,ecx
dls_line:
        mov     al,[esi+ecx]
        or      al,al
        jz      dls_lend
        cmp     al,13
        je      dls_lend
        cmp     al,10
        je      dls_lend
        cmp     ecx,DL_ARG_MAX - 3
        jae     dls_lend
        mov     [edi+ecx],al
        inc     ecx
        jmp     short dls_line
dls_lend:
        mov     byte ptr [edi+ecx],13   ; a driver looks for the CR, and
        mov     byte ptr [edi+ecx+1],10 ; some look for the zero
        mov     byte ptr [edi+ecx+2],0
        mov     [dl_linelen],ecx

        mov     edi,OFFSET dl_path      ; then the path out of the front
        xor     ecx,ecx
dls_path:
        mov     al,[esi+ecx]
        or      al,al
        jz      dls_pend
        cmp     al,' '
        jbe     dls_pend
        cmp     al,','
        je      dls_pend
        cmp     ecx,DL_PATH_MAX - 1
        jae     dls_pend
        mov     [edi+ecx],al
        inc     ecx
        jmp     short dls_path
dls_pend:
        mov     byte ptr [edi+ecx],0
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        cmp     byte ptr [dl_path],0    ; the count is the CALLER's ECX by
        je      dls_none                ; now: ask the buffer instead
        clc
        ret
dls_none:
        stc
        ret

;--- dl_setargs - the line, copied down where a driver can see it, and
;--- the far pointer to it worked out.
dl_setargs:
        pushad
        mov     esi,OFFSET dl_line
        mov     edi,[dl_seg]
        and     edi,0FFFFh
        shl     edi,4
        add     edi,DL_ARG
        mov     ecx,DL_ARG_MAX
        rep     movsb
        popad
        ret

;=====================================================================
; dl_doinit - INIT for the header at [dl_hdrflat].  CF if the driver
;             refused to install.
;
; The packet carries the command line in and the resident end out.  A
; character driver reporting units is reporting nonsense and is
; ignored rather than argued with; a block one is refused before it
; ever gets here.
;=====================================================================
dl_doinit:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        call    dl_enter
        jc      dld_bad
        mov     [dl_pktflat],edi
        mov     [dl_pktoff],ebx
        mov     eax,ebx
        add     eax,DL_STK
        mov     [dl_stktop],eax

        mov     byte ptr [edi+REQ_CMD],DEVC_INIT
        mov     byte ptr [edi+REQ_LEN],REQI_LEN
        mov     word ptr [edi+REQI_ARGOFF],DL_ARG
        mov     eax,[dl_seg]
        mov     word ptr [edi+REQI_ARGSEG],ax
        mov     byte ptr [edi+REQI_DRIVE],0

        call    dl_fire
        jc      dld_bad

        mov     esi,[dl_pktflat]
        movzx   eax,word ptr [esi+REQI_ENDOFF]
        mov     [dl_endoff],eax
        movzx   eax,word ptr [esi+REQI_ENDSEG]
        mov     [dl_endseg],eax

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
dld_bad:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; dl_chain - give the header at [dl_hdrflat] a name in dl_tab and link
;            it into the real chain behind NUL.
;
; BEHIND NUL, and that is not an arbitrary place: the list of lists
; anchors on NUL, every device walker starts there, and DOS itself put
; NUL first so that a loaded driver could go in immediately after it
; without moving the anchor.  Newest first is also what makes a second
; ANSI.SYS shadow the first, which is what DOS does.
;=====================================================================
dl_chain:
        pushad
        mov     eax,[dl_count]
        cmp     eax,DL_MAX
        jae     dlc_full
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        mov     edx,eax                 ; EDX = the new row

        mov     esi,[dl_hdrflat]
        add     esi,DEVH_NAME
        mov     edi,edx
        mov     ecx,8
dlc_name:
        mov     al,[esi]
        cmp     al,'a'
        jb      dlc_nolc
        cmp     al,'z'
        ja      dlc_nolc
        sub     al,20h
dlc_nolc:
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     dlc_name

        mov     eax,[dl_drvseg]
        mov     [edx+DLE_SEG],ax
        mov     eax,[dl_hdroff]
        mov     [edx+DLE_HDR],ax
        inc     dword ptr [dl_count]

        ; The link.  NUL's next pointer becomes ours and ours becomes
        ; what NUL's was, so the chain stays whole from either end.
        movzx   edi,word ptr ds:[vdm_lolseg]
        shl     edi,4
        add     edi,GLOL_HNUL
        mov     eax,[edi+DEVH_NEXT]
        mov     esi,[dl_hdrflat]
        mov     [esi+DEVH_NEXT],eax
        mov     eax,[dl_drvseg]
        shl     eax,16
        mov     ax,word ptr [dl_hdroff]
        mov     [edi+DEVH_NEXT],eax

        popad
        clc
        ret
dlc_full:
        popad
        stc
        ret

;=====================================================================
; dl_lookup - the eight blank-padded characters at dev_name, against
;             the loaded drivers.  CF clear: EAX = the device id.
;
; dev_find asks this BEFORE it reads its own table, and that order is
; the whole point of loading a driver: ANSI.SYS is a CON driver, and a
; CON that loses to the built-in console is a CON that does nothing.
;=====================================================================
dl_lookup:
        push    ebx
        push    ecx
        push    esi
        push    edi
        xor     ebx,ebx
dlk_try:
        cmp     ebx,[dl_count]
        jae     dlk_no
        mov     eax,ebx
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        mov     esi,eax
        mov     edi,OFFSET dev_name
        mov     ecx,8
        repe    cmpsb
        je      dlk_hit
        inc     ebx
        jmp     short dlk_try
dlk_hit:
        mov     eax,ebx
        imul    eax,DEV_UNIT
        add     eax,DEV_LOADED
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        clc
        ret
dlk_no:
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; dl_out - write AL to loaded device number DH.  No error return, in
;          keeping with every other dev_out path: the DOS character
;          output calls this serves have never had one.
;
; NO ERROR RETURN IS NOT THE SAME AS NO ERROR, and until v0.41 this
; routine treated them as the same thing: it called the driver, the
; driver set DEVS_ERROR and a reason, and the reason went nowhere.  A
; printer driver reporting out of paper and one reporting success were
; indistinguishable from here, which is the exact situation INT 24h
; was invented for.  Now the reason goes to the handler, and Retry
; comes back here and does the write again - which is what makes
; "put paper in and press R" work for a character device the same way
; it has worked for a disk since v0.20.
;
; The row is kept in a variable rather than in DH, because a retry has
; to start from the same place twice and DH belongs to a caller who
; has no idea a driver is involved.  Same argument as dio_try.
;=====================================================================
dl_out:
        pushad
        mov     [dl_byte],al
        movzx   eax,dh
        mov     [dl_row],eax
dlo_try:
        mov     eax,[dl_row]
        mov     ecx,DEVC_WRITE
        call    dl_prep
        jc      dlo_out
        mov     eax,[dl_pktoff]
        add     eax,DL_PKTLEN - 8       ; the byte itself, inside the packet
        mov     word ptr [edi+REQC_BUFOFF],ax
        mov     eax,[dl_seg]
        mov     word ptr [edi+REQC_BUFSEG],ax
        mov     word ptr [edi+REQC_COUNT],1
        mov     al,[dl_byte]
        mov     [edi+DL_PKTLEN-8],al
        call    dl_fire
        jnc     dlo_out
        call    dl_crit                 ; -> AL = the answer
        cmp     al,CERR_RETRY
        je      dlo_try
dlo_out:
        popad
        ret

;---------------------------------------------------------------------
; dl_crit - AX = the status word dl_fire came back with.  Raise the
;           critical error and return the handler's answer in AL.
;
; Both callers reach this having lost nothing yet, which is why the
; header pointer is read out of dl_hdrflat rather than saved: dl_prep
; set it for this row and a retry sets it again.
;---------------------------------------------------------------------
dl_crit:
        push    ebx
        push    esi
        and     eax,0FFh                ; the driver's own reason, which is
        mov     esi,[dl_hdrflat]        ; already a DOS error code
        mov     ebx,[dl_drvseg]         ; ...and the same header as a guest's
        shl     ebx,16                  ; handler has to be shown it: BP:SI,
        or      ebx,[dl_hdroff]         ; which is a segment and an offset
        call    crit_dev
        pop     esi
        pop     ebx
        ret

;=====================================================================
; dl_in - read one byte from loaded device number DH.  -> AL, CF set
;         at end of input.
;
; A driver that comes back having transferred nothing has nothing to
; give, and that is end of file - the same answer NUL and the printer
; give, and the one "PROG < DEVICE" depends on.
;=====================================================================
dl_in:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        movzx   eax,dh
        mov     [dl_row],eax
dlin_try:
        mov     eax,[dl_row]
        mov     ecx,DEVC_READ
        call    dl_prep
        jc      dlin_none
        mov     eax,[dl_pktoff]
        add     eax,DL_PKTLEN - 8
        mov     word ptr [edi+REQC_BUFOFF],ax
        mov     eax,[dl_seg]
        mov     word ptr [edi+REQC_BUFSEG],ax
        mov     word ptr [edi+REQC_COUNT],1
        call    dl_fire
        jnc     dlin_got

        ; AN ERROR AND AN EMPTY DEVICE ARE DIFFERENT THINGS and only
        ; one of them is worth asking about.  dl_fire's carry is the
        ; driver saying it went wrong; a driver that came back clean
        ; having moved nothing is end of input, and falls out below.
        call    dl_crit
        cmp     al,CERR_RETRY
        je      dlin_try
        jmp     short dlin_none
dlin_got:
        mov     edi,[dl_pktflat]
        cmp     word ptr [edi+REQC_COUNT],0
        je      dlin_none
        movzx   eax,byte ptr [edi+DL_PKTLEN-8]
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
dlin_none:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; state
;=====================================================================
dl_seg      dd  0                       ; the scratch block, or 0
dl_row      dd  0                       ; whose driver, kept so a retry
                                        ; can ask the same one twice
dl_depth    dd  0
dl_count    dd  0                       ; how many names are in dl_tab
dl_drvseg   dd  0
dl_hdroff   dd  0
dl_hdrflat  dd  0
dl_pktflat  dd  0
dl_pktoff   dd  0
dl_stktop   dd  0
dl_entry    dd  0
dl_sft      dd  0
dl_fsize    dd  0
dl_got      dd  0
dl_err      dd  0
dl_linelen  dd  0
dl_fnext    dd  0
dl_endoff   dd  0
dl_endseg   dd  0
dl_lost     db  0
dl_isblk    db  0
dl_byte     db  0
            dw  0
dl_path     db  DL_PATH_MAX dup (0)
dl_line     db  DL_ARG_MAX dup (0)
dl_tab      db  DL_MAX * DLE_LEN dup (0)

; block drivers
dl_nunits   dd  0                       ; driver units that got a letter
dl_bunits   dd  0                       ; units the driver being loaded has
dl_bpbtab   dd  0                       ; its BPB array, flat
dl_bpb      dd  0                       ; and the one unit being looked at
dl_stgseg   dd  0                       ; the staging block, or 0
dl_iorow    dd  0                       ; the transfer in progress
dl_iocnt    dd  0
dl_iothis   dd  0
dl_iolba    dd  0
dl_iobuf    dd  0
dl_iowr     db  0                       ; ...and which way it goes, out of
            db  0,0,0                   ; the SERVICE the caller named
dl_units    db  DLU_MAX * DLU_LEN dup (0)
;=====================================================================
; BLOCK DRIVERS
;
; A character driver is a name; a BLOCK driver is a DRIVE LETTER, and
; that difference is the whole of the extra work.  ANSI.SYS joins the
; device chain and dev_find starts finding it.  RAMDRIVE.SYS hands
; back a unit count and a BPB for each unit, and every one of those
; units has to become a letter in a volume table that until now got
; its letters by scanning partitions on real disks.
;
; THREE THINGS A BLOCK DRIVER NEEDS THAT A CHARACTER ONE DOES NOT.
;
; A ROW IN drv_tbl, so that the filesystem can be pointed at it.  The
; table is keyed by BIOS drive number, which a driver unit does not
; have, so it is given one: DL_UNIT_BASE upwards, a range no BIOS ever
; uses (this machine's real disks are 80h and 81h).  The number then
; flows through fat_mount, disk_read and bios2log untouched, and the
; only place that has to know what it means is dio_try.  Inventing a
; key is cheaper than threading a second one through nine routines.
;
; A STAGING BUFFER.  A driver is 16-bit real-mode code and takes the
; address of its transfer as a segment and an offset, and the buffers
; the filesystem hands down are sec_buf and clus_buf inside PMDOS.SYS
; at 110000h, which no segment:offset can name.  So every transfer is
; copied through a block of conventional memory, in chunks, exactly as
; the floppy path has staged through XFER_BUF since v0.9.  It costs a
; memcpy per sector and it is the only arrangement that works.
;
; AND A SECTOR NUMBER THAT FITS.  The request packet's sector field is
; a WORD - the format is from 1983 and 32MB was a large disk - so a
; volume past 65535 sectors needs the DOS 4 form: FFFFh in the word
; and a dword after the packet.  A driver says it understands that by
; setting bit 1 of its attribute word, and one that does not is held
; to what it can address rather than being sent a sector number it
; will silently truncate.
;=====================================================================

;---------------------------------------------------------------------
; dl_blkinit - a block driver's INIT has come back.  Give each unit a
;              drive letter.  ESI -> the packet.  CF if none of them
;              could be taken.
;
; THE UNIT COUNT IS THE DRIVER'S ANSWER AND NOT ITS HEADER'S.  A block
; header carries a unit count in the first byte of the name field, but
; it is the count BEFORE init - the driver may find less hardware than
; it hoped - and DOS uses the packet's, which is what was written
; after the driver looked.
;---------------------------------------------------------------------
dl_blkinit:
        pushad
        movzx   ecx,byte ptr [esi+REQI_UNITS]
        or      ecx,ecx
        jz      dbi_none                ; a driver that found nothing
        mov     [dl_bunits],ecx

        ; The BPB array: a far pointer to N far pointers, one per unit.
        movzx   eax,word ptr [esi+REQI_ARGSEG]
        shl     eax,4
        movzx   edx,word ptr [esi+REQI_ARGOFF]
        add     eax,edx
        mov     [dl_bpbtab],eax

        xor     ebx,ebx                 ; unit number
dbi_unit:
        cmp     ebx,[dl_bunits]
        jae     dbi_done
        cmp     dword ptr [dl_nunits],DLU_MAX
        jae     dbi_done                ; no more pseudo-drives to give

        ; this unit's BPB, through the array
        mov     eax,[dl_bpbtab]
        mov     eax,[eax+ebx*4]
        mov     edx,eax
        shr     edx,16
        shl     edx,4
        movzx   eax,ax
        add     eax,edx                 ; EAX -> the BPB, flat
        mov     [dl_bpb],eax

        ; how big is it?  The 16-bit total, or the 32-bit one behind it
        ; when the 16-bit field is zero - which is how every BPB since
        ; DOS 4 says "bigger than 65535 sectors".
        movzx   ecx,word ptr [eax+DBPB_TOTSEC16]
        or      ecx,ecx
        jnz     dbi_size
        mov     ecx,[eax+DBPB_TOTSEC32]
dbi_size:
        or      ecx,ecx
        jz      dbi_next                ; a unit with no sectors in it

        ; a row in dl_units, and the pseudo BIOS number that names it
        mov     edi,[dl_nunits]
        imul    edi,DLU_LEN
        add     edi,OFFSET dl_units
        mov     eax,[dl_count]
        dec     eax                     ; dl_chain has already added ours
        mov     [edi+DLU_TAB],eax
        mov     [edi+DLU_UNIT],bl
        mov     eax,[dl_bpb]
        mov     al,[eax+DBPB_MEDIA]
        mov     [edi+DLU_MEDIA],al

        ; and a drive letter
        mov     eax,[dl_nunits]
        add     eax,DL_UNIT_BASE
        xor     ah,ah                   ; NO PARTITION TYPE, which is what a
        xor     edx,edx                 ; zero already means in this table -
                                        ; "the whole medium".  Claiming
                                        ; FAT16B for a unit whose BPB says
                                        ; FAT12 put a contradiction in two
                                        ; adjacent columns of DRIVES.
        push    ebx
        mov     bl,0FFh                 ; no partition entry behind it
        call    drv_add
        pop     ebx
        jc      dbi_done                ; out of letters

        movzx   edi,byte ptr [drv_count]        ; the row it just took
        dec     edi
        imul    edi,DRV_LEN
        add     edi,OFFSET drv_tbl
        or      byte ptr [edi+DRV_FLAGS],DRVF_DRIVER

        inc     dword ptr [dl_nunits]
dbi_next:
        inc     ebx
        jmp     short dbi_unit
dbi_done:
        popad
        clc
        ret
dbi_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dl_stage - the conventional buffer every block transfer is copied
;            through.  Taken the first time a block driver loads and
;            never given back.  CF if there is no room for it.
;---------------------------------------------------------------------
dl_stage:
        cmp     dword ptr [dl_stgseg],0
        jne     dst_have
        pushad
        mov     ebx,DL_IOSECS * 512 / 16
        call    lm_alloc
        jc      dst_fail
        movzx   eax,ax
        mov     [dl_stgseg],eax
        mov     dx,8                    ; the system's, so that a program
        call    lm_setown               ; exiting never takes it back
        popad
dst_have:
        clc
        ret
dst_fail:
        popad
        stc
        ret

;=====================================================================
; dl_blkio - one transfer for a driver unit.
;
;   AL = the pseudo BIOS drive (DL_UNIT_BASE upwards)
;   EBX = LBA within the unit, CX = sectors, EDI = flat buffer
;   EDX = IOS_RDBLK or IOS_WRBLK, which is the direction
;
;   CF set if the driver could not do it.
;
; THE DIRECTION IS THE SERVICE AND NOT [dio_wr], which it was until
; v0.42.  dio_wr describes the transfer disk_write is retrying, and
; that is very nearly the same thing - it was the same thing while
; disk_write was the only caller.  It stopped being one the moment
; VERIFY started reading a sector back in the middle of a write, at
; which point "what is this transfer" and "what was the call" are
; opposite answers and the read went out as a second write.
;
; Chunked by the staging buffer, and the chunk loop is also what makes
; a short transfer honest: the driver reports how many sectors it
; really moved in the packet's count field, and a driver that moved
; fewer than it was asked has hit the end of the medium or a bad one.
; Stopping there and reporting failure is what lets INT 24h see it.
;=====================================================================
dl_blkio:
        pushad
        mov     byte ptr [dl_iowr],0
        cmp     edx,IOS_WRBLK
        jne     dbio_dir
        mov     byte ptr [dl_iowr],1
dbio_dir:
        call    dl_stage
        jc      dbio_fail

        movzx   eax,al
        sub     eax,DL_UNIT_BASE
        cmp     eax,[dl_nunits]
        jae     dbio_fail
        imul    eax,DLU_LEN
        add     eax,OFFSET dl_units
        mov     [dl_iorow],eax

        movzx   ecx,cx
        mov     [dl_iocnt],ecx
        mov     [dl_iolba],ebx
        mov     [dl_iobuf],edi

dbio_chunk:
        cmp     dword ptr [dl_iocnt],0
        je      dbio_done
        mov     ecx,[dl_iocnt]
        cmp     ecx,DL_IOSECS
        jbe     dbio_size
        mov     ecx,DL_IOSECS
dbio_size:
        mov     [dl_iothis],ecx

        ; a write fills the staging buffer before the driver sees it
        cmp     byte ptr [dl_iowr],0
        je      dbio_build
        mov     esi,[dl_iobuf]
        mov     edi,[dl_stgseg]
        shl     edi,4
        mov     ecx,[dl_iothis]
        shl     ecx,7                   ; sectors -> dwords
        rep     movsd

dbio_build:
        mov     eax,[dl_iorow]
        mov     eax,[eax+DLU_TAB]
        mov     ecx,DEVC_READ
        cmp     byte ptr [dl_iowr],0
        je      dbio_cmd
        mov     ecx,DEVC_WRITE
dbio_cmd:
        call    dl_prep
        jc      dbio_fail

        mov     eax,[dl_iorow]
        mov     dl,[eax+DLU_UNIT]
        mov     [edi+REQ_UNIT],dl
        mov     dl,[eax+DLU_MEDIA]
        mov     [edi+REQC_MEDIA],dl
        mov     word ptr [edi+REQC_BUFOFF],0
        mov     eax,[dl_stgseg]
        mov     word ptr [edi+REQC_BUFSEG],ax
        mov     eax,[dl_iothis]
        mov     word ptr [edi+REQC_COUNT],ax

        ; THE SECTOR NUMBER, AND THE TWO FORMS OF IT
        mov     eax,[dl_iolba]
        cmp     eax,0FFFFh
        jb      dbio_small
        call    dl_islarge              ; will this driver understand it?
        jc      dbio_toobig
        mov     word ptr [edi+REQC_SECTOR],0FFFFh
        mov     [edi+REQC_HUGE],eax
        mov     byte ptr [edi+REQ_LEN],REQC_HUGELEN
        jmp     short dbio_fire
dbio_small:
        mov     word ptr [edi+REQC_SECTOR],ax
dbio_fire:
        call    dl_fire
        jc      dbio_fail

        ; how many did it really move?
        mov     edi,[dl_pktflat]
        movzx   eax,word ptr [edi+REQC_COUNT]
        cmp     eax,[dl_iothis]
        jb      dbio_fail               ; short: the medium ended, or is bad

        ; a read empties the staging buffer afterwards
        cmp     byte ptr [dl_iowr],0
        jne     dbio_moved
        mov     esi,[dl_stgseg]
        shl     esi,4
        mov     edi,[dl_iobuf]
        mov     ecx,[dl_iothis]
        shl     ecx,7
        rep     movsd

dbio_moved:
        mov     eax,[dl_iothis]
        mov     ecx,eax
        shl     ecx,9                   ; sectors -> bytes
        add     [dl_iobuf],ecx
        add     [dl_iolba],eax
        sub     [dl_iocnt],eax
        jmp     dbio_chunk

dbio_done:
        popad
        clc
        ret
dbio_toobig:
        call    dl_leave                ; dl_prep took a level: give it back
dbio_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dl_islarge - CF clear if the unit in [dl_iorow] belongs to a driver
;              that understands 32-bit sector numbers.
;
; Bit 1 of a block driver's attribute word, and it is worth checking
; rather than assuming: a driver written before DOS 4 reads the word
; at REQC_SECTOR and would take FFFFh as a sector number, which is a
; transfer to the wrong place reported as a success.
;---------------------------------------------------------------------
dl_islarge:
        push    eax
        push    esi
        mov     eax,[dl_iorow]
        mov     eax,[eax+DLU_TAB]
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        movzx   esi,word ptr [eax+DLE_SEG]
        shl     esi,4
        movzx   eax,word ptr [eax+DLE_HDR]
        add     esi,eax
        test    word ptr [esi+DEVH_ATTR],DEV_ATTR_BIG
        jz      dil_no
        pop     esi
        pop     eax
        clc
        ret
dil_no:
        pop     esi
        pop     eax
        stc
        ret

