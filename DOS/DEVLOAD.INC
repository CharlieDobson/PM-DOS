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
DLERR_BLOCK     EQU     3
DLERR_INIT      EQU     4
DLERR_FULL      EQU     5

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

dlx_run:
        mov     esi,OFFSET vdm_ctx
        call    dword ptr ds:[IOS_V86RUN]
        cmp     eax,V86R_HLT
        je      dlx_halt
        cmp     eax,V86R_INT
        jne     dlx_lost                ; a fault inside a driver is not
        cmp     ebx,21h                 ; ours to recover from
        jne     dlx_lost
        mov     al,byte ptr [vdm_ctx+V86C_EAX+1]
        cmp     al,4Ch                  ; a driver that TERMINATES has gone
        je      dlx_lost                ; wrong: there is no process here
        cmp     al,31h
        je      dlx_lost
        mov     al,byte ptr [vdm_ctx+V86C_EAX+1]
        call    vdm_do21
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

        test    word ptr [esi+DEVH_ATTR],DEV_ATTR_CHAR
        jz      dll_block               ; a block driver: not this release

        call    dl_doinit
        jc      dll_initbad
        call    dl_chain
        jc      dll_full

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
dll_block:
        mov     eax,DLERR_BLOCK
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
;=====================================================================
dl_out:
        pushad
        mov     [dl_byte],al
        movzx   eax,dh
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
dlo_out:
        popad
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
        jc      dlin_none
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
dl_byte     db  0
            dw  0
dl_path     db  DL_PATH_MAX dup (0)
dl_line     db  DL_ARG_MAX dup (0)
dl_tab      db  DL_MAX * DLE_LEN dup (0)
