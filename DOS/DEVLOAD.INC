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
;
; A 32-BIT DRIVER'S UNIT (NDRV.INC, NDTF_DISK) is a row here too, with
; the same pseudo number and the same DRVF_DRIVER letter, so that
; everything that asks "is this a driver's drive?" gets the right answer
; without being told there are two kinds.  Only the transfer, and the
; park, look at the kind: such a unit is called, not sent packets.
DL_UNIT_BASE    EQU     0E0h            ; pseudo BIOS drive numbers for them
DLU_MAX         EQU     16              ; driver units, in total
DLU_TAB         EQU     0               ; dd - which dl_tab row, or for a
                                        ; 32-bit unit which nd_tab row
DLU_UNIT        EQU     4               ; db - which unit of that driver
DLU_MEDIA       EQU     5               ; db - its media descriptor
DLU_KIND        EQU     6               ; db - DLK_*
                                        ; db - spare
DLU_IO          EQU     8               ; dd - DLK_D32: the driver's NDU_IO
DLU_REC         EQU     12              ; dd - DLK_D32: its NDU record
DLU_SECS        EQU     16              ; dd - DLK_D32: the unit's size, and
DLU_MEM         EQU     20              ; dd - where its sectors are, for an
                                        ; NDUF_MEMORY unit, else 0.  These
                                        ; two are side by side on purpose:
                                        ; RMDOS reads them as one (PARK.INC)
DLU_LEN         EQU     24
DLK_RM          EQU     0               ; a real-mode driver's, by packets
DLK_D32         EQU     1               ; a 32-bit driver's, by a call
DL_IOSECS       EQU     16              ; the staging buffer, in sectors

DL_PATH_MAX     EQU     80
DLX_NV86        EQU     10              ; DOS calls a driver's excursion
                                        ; hands to the box's own handlers

; The scratch block, and everything in it is there because a real-mode
; driver has to be able to reach it with a segment and an offset:
;
;   0000  the return pad - one HLT
;   0004  INT 2Fh and RETF, for dl_conprobe
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
DL_PROBE        EQU     0004h           ; INT 2Fh and RETF, for dl_conprobe
DL_ARG          EQU     0010h
DL_ARG_MAX      EQU     0080h
DL_BASE         EQU     0090h
DL_PKTLEN       EQU     0020h
DL_STRIDE       EQU     0120h           ; the packet and its stack
DL_STK          EQU     0120h           ; stack top, measured from the packet
DL_XBUF         EQU     DL_BASE + DL_LEVELS * DL_STRIDE
DL_XBUFLEN      EQU     0200h           ; an IOCTL transfer for a native
                                        ; program, whose buffer is above the
                                        ; megabyte - see dl_ioctl
DL_SIZE         EQU     DL_XBUF + DL_XBUFLEN
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

        ; THE PER-CALL DRIVER STATE IS A SET OF SINGLE GLOBALS.  dl_prep
        ; and dl_doinit write them; dl_go and dl_fire read them back
        ; AFTER this excursion returns - dl_fire reads dl_pktflat for the
        ; status, dl_go re-reads dl_hdrflat for the interrupt entry.  A
        ; driver's INIT that makes a DOS call which reaches ANOTHER loaded
        ; driver nests a whole driver call inside this one: an NDIS MAC's
        ; INIT doing an IOCTL to the Protocol Manager is exactly that, and
        ; the nested dl_prep rewrites every one of these.  Park them on
        ; the stack - one copy per level, so up to DL_LEVELS deep - and
        ; the nesting is invisible to the level above.  Console output
        ; during INIT nests too, but goes through the vector stubs rather
        ; than dl_fire and so never tripped this; an IOCTL has no such
        ; fast path and is the first call that does.
        push    dword ptr [dl_entry]
        push    dword ptr [dl_drvseg]
        push    dword ptr [dl_hdroff]
        push    dword ptr [dl_hdrflat]
        push    dword ptr [dl_pktflat]
        push    dword ptr [dl_pktoff]
        push    dword ptr [dl_stktop]

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
        mov     eax,[dl_inax]           ; 0, but for dl_conprobe's INT 2Fh
        mov     [edi+V86C_EAX],eax
        mov     dword ptr [dl_inax],0
        mov     dword ptr [edi+V86C_FLAGS],EFL_IF       ; V86_UFLAGS is a MASK, not
                                                ; a value: as a value it sets DF

        ; The far return address, on the driver's own stack.  RETF
        ; pops the offset first, so the pad's offset goes lower.
        ;
        ; AND BENEATH IT, WHEN dl_go ASKS, THE INTERRUPT ENTRY: the
        ; strategy routine's RETF lands on it, and the interrupt
        ; routine's RETF on the pad.  One trip into V86 for a request
        ; instead of two, which matters once a loaded console is
        ; spoken to for every character on the screen.  The interrupt
        ; routine starts with whatever registers strategy left, which
        ; is no more than DOS ever promised it.
        mov     eax,[dl_stktop]
        sub     eax,4
        cmp     dword ptr [dl_then],-1
        je      dlx_onefar
        sub     eax,4
dlx_onefar:
        mov     [edi+V86C_ESP],eax
        mov     esi,[dl_seg]
        and     esi,0FFFFh
        shl     esi,4
        add     esi,eax
        cmp     dword ptr [dl_then],-1
        je      dlx_pad
        mov     eax,[dl_then]
        mov     word ptr [esi],ax
        mov     eax,[dl_drvseg]
        mov     word ptr [esi+2],ax
        add     esi,4
        mov     dword ptr [dl_then],-1  ; spent: a nested call has its own
dlx_pad:
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
dlx_i21:
        mov     al,byte ptr [vdm_ctx+V86C_EAX+1]
        cmp     al,4Ch                  ; a driver that TERMINATES has gone
        je      dlx_lost                ; wrong: there is no process here
        cmp     al,31h
        je      dlx_lost

        ; THE CALLS WITH A V86 ANSWER OF THEIR OWN go where a box's go.
        ; vdm_do21 is the KERNEL's INT 21h, and for these that is the
        ; wrong one: its 25h points a protected-mode IDT gate at the
        ; driver's segment:offset and leaves the real-mode vector table
        ; - the one the interrupt is delivered through - as it was, so
        ; a driver hooking its card's IRQ at INIT never heard from the
        ; card (ASPI4DOS waited two minutes per device for an interrupt
        ; the BIOS's spurious-interrupt handler took and then masked);
        ; 35h, 34h, 52h and the rest answer flat addresses a driver
        ; cannot hold.  The box's handlers all end at vdm_loop, which
        ; sees vdm_inexc and comes back to dlx_back21.
        mov     edi,OFFSET dlx_v86fn
        mov     ecx,DLX_NV86
        cld
        repne   scasb
        je      dlx_v86call
        mov     al,byte ptr [vdm_ctx+V86C_EAX+1]
        call    vdm_do21
        jmp     short dlx_run
dlx_v86call:
        mov     byte ptr ds:[vdm_inexc],1
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX+1]
        jmp     vdm_dos_v86
dlx_back21:
        mov     byte ptr ds:[vdm_inexc],0
        jmp     dlx_run

dlx_ems:
        call    ems_call
        jmp     short dlx_run

;--- the HLT.  WHICH one?  Only the pad ends the excursion; a HLT
;--- anywhere else is a driver that has stopped in the middle of
;--- itself, and carrying on would run whatever follows it.
;
;--- EXCEPT A DOS VECTOR'S STUB.  2Fh, 25h, 26h and 29h go through the
;--- vector table here as they do in a box (vdm_stubs), and so does an
;--- INT 21h a driver hooked during its INIT and then chained on; those
;--- are served as a box's are.  Ending, going resident and ^C are not
;--- a driver's to do, and are what they always were: lost.
dlx_halt:
        call    dvs_which
        jc      dlx_nodvs
        cmp     eax,21h
        je      dlx_s21
        cmp     eax,2Fh
        je      dlx_s2F
        cmp     eax,29h
        je      dlx_s29
        xor     edx,edx
        cmp     eax,25h
        je      dlx_sabs
        inc     edx
        cmp     eax,26h
        je      dlx_sabs
        jmp     dlx_lost
dlx_s21:
        call    vdm_ciret
        jmp     dlx_i21
dlx_s2F:
        call    vdm_ciret
        call    dvs_do2F
        mov     byte ptr ds:[win_parkreq],0     ; nothing parks from an INIT
        jmp     dlx_run
dlx_s29:
        call    vdm_ciret
        call    dvs_do29
        jmp     dlx_run
dlx_sabs:
        call    vdm_cretf
        call    dvs_doabs
        jmp     dlx_run
dlx_nodvs:
        ; ONE OF PMIO'S OWN STUBS - Virtual DMA Services' INT 4Bh, or the
        ; mouse's INT 33h - served as a box's run loop serves them.  A
        ; driver that masters the bus has to ask VDS where its buffer
        ; physically is, and an ASPI manager asks on every request it
        ; is handed (ASPI.INC); it used to find the HLT taken for the
        ; driver stopping in the middle of itself
        mov     esi,OFFSET vdm_ctx
        movzx   eax,word ptr ds:[vdm_pspseg]
        or      eax,MSTOK_BOX
        call    dword ptr ds:[IOS_MSHLT]
        jnc     dlx_run
        call    ems_iscb                ; and EMS 56h's return, for a driver
        jc      dlx_noems               ; that calls an overlay of its own
        call    ems_callret
        jc      dlx_lost
        jmp     dlx_run
dlx_noems:
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
        mov     eax,dword ptr [vdm_ctx+V86C_EAX]        ; for dl_conprobe
        mov     [dl_outax],eax
        mov     esi,esp                 ; and the parked context back
        mov     edi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        rep     movsd
        add     esp,V86C_SIZE

        ; the per-call driver state the level above is owed back, in the
        ; reverse of the order it was parked
        pop     dword ptr [dl_stktop]
        pop     dword ptr [dl_pktoff]
        pop     dword ptr [dl_pktflat]
        pop     dword ptr [dl_hdrflat]
        pop     dword ptr [dl_hdroff]
        pop     dword ptr [dl_drvseg]
        pop     dword ptr [dl_entry]

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
;
; IN ONE EXCURSION since ANSI.SYS (dl_then, in dl_excur): strategy
; returns into interrupt.  A strategy routine that goes wrong is lost
; before interrupt runs, as it was when they were two.
;=====================================================================
dl_go:
        pushad
        mov     esi,[dl_hdrflat]
        movzx   eax,word ptr [esi+DEVH_STRAT]
        mov     [dl_entry],eax
        movzx   eax,word ptr [esi+DEVH_INTR]
        mov     [dl_then],eax
        call    dl_excur
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
        ; dl_lost is one global, and a nested driver call borrows it: it
        ; zeroes it, runs, and leaves it set if THAT driver went wrong.
        ; The level above then reads a 1 it never earned.  Park the
        ; caller's value, clear it for this call, and give it back once
        ; this call's own result is in hand.  EDX is preserved because a
        ; caller is entitled to expect dl_fire to touch only EAX and EDI.
        push    edx
        movzx   edx,byte ptr [dl_lost]
        push    edx
        mov     byte ptr [dl_lost],0
        call    dl_go
        call    dl_leave
        movzx   eax,byte ptr [dl_lost]  ; this call's own lost-state in AL
        pop     edx                     ; the caller's, back where it was
        mov     byte ptr [dl_lost],dl
        pop     edx
        mov     edi,[dl_pktflat]
        ; AX = the packet status word on EVERY return, lost or not, so
        ; dl_crit reads the driver's reason from it as it always has.
        ; MOVZX leaves the flags alone, so the lost test below still sees
        ; its own ZF after AX has been reloaded.
        test    al,al                   ; did THIS call go off the rails?
        movzx   eax,word ptr [edi+REQ_STATUS]
        jnz     dlf_err
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

        ; ALL THE ROOM THERE IS, AND INIT IS TOLD WHERE IT ENDS.  A
        ; driver is loaded at the bottom of free memory with the rest
        ; of it above, and the packet says how far that reaches; INIT
        ; builds its tables and buffers past the end of its own image
        ; and answers with where it stopped, which dll_keep trims the
        ; block to.  An ASPI manager is exactly that driver: given a
        ; block the size of its file and an end of 0:0, ASPI4DOS says
        ; "Not enough memory available to load driver" and refuses.
        ; lm_allocbig asks the regions in the strategy's order, so
        ; DEVICEHIGH still means upper memory first.
        call    lm_allocbig             ; at least the file; the largest
        jc      dll_nomem_c             ; block there is
        movzx   eax,ax
        mov     [dl_drvseg],eax
        mov     edx,eax
        dec     edx
        shl     edx,4
        movzx   edx,word ptr [edx+RMCB_LEN]     ; how big it came out
        add     edx,eax
        mov     [dl_brkseg],edx                 ; the paragraph after it
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
        call    dl_mzfix                ; an .EXE: header off, fixups in
        jc      dll_initbad

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

        ; WHAT 30h TELLS THE DRIVER WHILE ITS INIT RUNS: SETVER's entry
        ; for the file's name, which MS-DOS puts in SYSINIT's PSP for the
        ; same few minutes.  The table has .SYS names in it for this.
        push    esi
        mov     esi,OFFSET dl_path
        call    sv_find
        mov     word ptr ds:[sv_initver],ax
        pop     esi
        call    dl_doinit
        mov     word ptr ds:[sv_initver],0      ; a MOV: CF survives it
        jc      dll_initbad
        call    dl_chain
        jc      dll_full

        cmp     byte ptr [dl_isblk],0
        jne     dll_isblk
        call    dl_conadopt             ; ANSI.SYS: the console from now on
        jmp     short dll_notblk
dll_isblk:
        mov     esi,[dl_pktflat]        ; INIT's answers are still in it
        call    dl_blkinit
        jc      dll_nounits
dll_notblk:

        mov     eax,dword ptr [dl_fnext]
        cmp     ax,0FFFFh
        je      dll_keep
        movzx   eax,ax
        mov     [dl_hdroff],eax
        jmp     dll_hdr                 ; near: MASM will not stretch a short one

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
; dl_mzfix - the file at [dl_drvseg] has been read in whole, [dl_fsize]
;            bytes.  If it is an .EXE, apply its fixups for the segment
;            it sits at and move its image down over the header, leaving
;            [dl_fsize] the image's length.  A .SYS is left alone.  CF
;            if the header does not describe the file.  Registers kept.
;
; AN .EXE CAN BE A DRIVER, and has been since DOS 5: the device header
; is the first thing in the image after the MZ header, and DOS loads it
; the way EXEC loads an overlay - the image only, relocated by the
; segment it lands at.  SETVER.EXE is the one every CONFIG.SYS of the
; period names, and it is the same file as the SETVER command.
;
; EVERY FIXUP IS CHECKED to land inside the image before it is applied,
; and the fixup table inside the file: a header is the file's word for
; it, and the words are added to memory that is about to run.
;---------------------------------------------------------------------
dl_mzfix:
        pushad
        mov     esi,[dl_drvseg]
        shl     esi,4                           ; ESI = the file, flat
        mov     ecx,[dl_fsize]
        cmp     ecx,EXE_HDRLEN
        jb      dmz_ok                          ; too short to be an .EXE
        mov     ax,[esi+EXE_SIG]
        cmp     ax,EXE_MZ
        je      dmz_exe
        cmp     ax,EXE_ZM
        jne     dmz_ok
dmz_exe:
        movzx   eax,word ptr [esi+EXE_CP]       ; pages of 512, the last
        shl     eax,9                           ; one CBLP long when CBLP
        movzx   edx,word ptr [esi+EXE_CBLP]     ; is not zero
        or      edx,edx
        jz      dmz_full
        sub     eax,512
        add     eax,edx
dmz_full:
        cmp     eax,ecx                         ; a header that claims more
        jbe     dmz_inside                      ; than the file holds gets
        mov     eax,ecx                         ; what there is, as EXEC does
dmz_inside:
        movzx   ebx,word ptr [esi+EXE_CPARHDR]
        shl     ebx,4                           ; EBX = the header's length
        sub     eax,ebx
        jbe     dmz_bad                         ; EAX = the image's length

        movzx   ecx,word ptr [esi+EXE_CRLC]
        jecxz   dmz_move
        movzx   edx,word ptr [esi+EXE_LFARLC]
        lea     edi,[edx+ecx*4]
        cmp     edi,[dl_fsize]
        ja      dmz_bad                         ; a table off the end
        add     edx,esi                         ; EDX -> the first fixup
dmz_fix:
        movzx   edi,word ptr [edx+2]            ; segment:offset within
        shl     edi,4                           ; the image
        movzx   ebp,word ptr [edx]
        add     edi,ebp
        lea     ebp,[edi+2]
        cmp     ebp,eax
        ja      dmz_bad                         ; a word outside the image
        add     edi,ebx
        add     edi,esi
        mov     bp,word ptr [dl_drvseg]         ; the image's own segment,
        add     [edi],bp                        ; once it has moved down
        add     edx,4
        loop    dmz_fix
dmz_move:
        mov     [dl_fsize],eax
        mov     ecx,eax
        mov     edi,esi
        add     esi,ebx
        cld
        rep     movsb                           ; down, so forwards is safe
dmz_ok:
        popad
        clc
        ret
dmz_bad:
        popad
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
        mov     word ptr [edi+REQI_ENDOFF],0    ; how far it may grow: the
        mov     eax,[dl_brkseg]                 ; end of the block dl_load
        mov     word ptr [edi+REQI_ENDSEG],ax   ; gave it, as DOS 5 says

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
;
; NEWEST FIRST, as the chain is (dl_chain), so a second driver of one
; name shadows the first.  And the driver the console was handed to
; (dl_conadopt) is DEV_CON itself: CON opened by name is the console,
; with the console's line editing and ^C, whose characters happen to
; go through a driver - not a device that merely shares its name.
;=====================================================================
dl_lookup:
        push    ebx
        push    ecx
        push    esi
        push    edi
        mov     ebx,[dl_count]
dlk_try:
        or      ebx,ebx
        jz      dlk_no
        dec     ebx
        mov     eax,ebx
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        mov     esi,eax
        mov     edi,OFFSET dev_name
        mov     ecx,8
        repe    cmpsb
        jne     dlk_try
        mov     eax,DEV_CON
        cmp     ebx,[dl_conrow]
        je      dlk_have
        mov     eax,ebx
        imul    eax,DEV_UNIT
        add     eax,DEV_LOADED
dlk_have:
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

;---------------------------------------------------------------------
; dl_iscon - DH = a loaded device's row.  ZF clear if its header says
;            it is standard output (attribute bit 1): ANSI.SYS, which
;            a write through 40h then treats as the screen it is.  All
;            registers preserved.
;---------------------------------------------------------------------
dl_iscon:
        push    eax
        push    ecx
        movzx   eax,dh
        cmp     eax,[dl_count]
        jae     dlc_no
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        movzx   ecx,word ptr [eax+DLE_SEG]
        shl     ecx,4
        movzx   eax,word ptr [eax+DLE_HDR]
        add     eax,ecx
        test    word ptr [eax+4],0002h
        jmp     short dlc_out
dlc_no:
        cmp     eax,eax                 ; ZF: not one
dlc_out:
        pop     ecx
        pop     eax
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
; A LOADED CONSOLE (2026-10-24): ANSI.SYS, and any driver like it
;
; Until now a driver named CON was found by name and nothing else: a
; program that opened "CON" got it, and standard input and output - the
; handles DOS hands every program, which are the kernel's console -
; never went near it.  So ANSI.SYS loaded and did nothing, and nobody
; had ever noticed, because nobody had loaded one.
;
; MS-DOS makes the newest driver that calls itself the console THE
; console (SYSINIT, sysconf.asm: the ISCIN bit puts it in the list of
; lists, and CON is opened by name for the standard handles).  Here the
; standard handles stay the kernel's console - its cooked and raw
; modes, line editing, ^C, ^S and ^P are DOS's and not a driver's, in
; MS-DOS as here - and what changes is the bottom of it: the character
; that reaches the screen, and the key that comes from the keyboard,
; go through the driver.  con_put, con_wrun, con_kst, con_krd and
; con_kflush are that bottom; everything above them is as it was.
;
; ONLY WITH ONE LOADED.  With no console driver every one of them is
; the call it replaced, so a machine without ANSI.SYS runs what it ran.
;
; NOT THE KERNEL'S OWN WORDS: a critical error's question, a CONFIG.SYS
; complaint, a crash report still go straight to the screen through
; IOS_CONOUT, as they must when the machine is in trouble.  And not a
; full-screen program's keys (21h/F007h), which want the scan codes the
; BIOS gives and a driver does not.  Under Windows RMDOS writes through
; the driver too (drv_coninit, RMDEV.INC) but reads the BIOS.
;=====================================================================

;---------------------------------------------------------------------
; dl_conadopt - the header at [dl_hdrflat] is a character driver, just
;               chained as the newest row.  Named CON and standard
;               output: it is the console from now on, and its keyboard
;               too if it says it is standard input.  The newest wins,
;               as DOS's does.  All registers preserved.
;---------------------------------------------------------------------
dl_conadopt:
        pushad
        mov     esi,[dl_hdrflat]
        test    word ptr [esi+DEVH_ATTR],0002h
        jz      dca_out
        mov     edx,[dl_count]
        dec     edx                     ; the row dl_chain gave it
        mov     esi,edx
        imul    esi,DLE_LEN
        add     esi,OFFSET dl_tab
        mov     edi,OFFSET dlc_conname
        mov     ecx,8
        repe    cmpsb                   ; the row's name, folded by dl_chain
        jne     dca_out
        mov     [dl_conrow],edx
        mov     esi,[dl_hdrflat]
        mov     al,byte ptr [esi+DEVH_ATTR]
        and     al,1
        mov     [dl_conin],al
        call    dl_conprobe
dca_out:
        popad
        ret

;---------------------------------------------------------------------
; dl_conprobe - INT 2Fh AX=1A00h, which is how MS-DOS's COMMAND.COM
;               asks whether ANSI.SYS is there before a CLS: AL = FFh
;               means a screen that ESC[2J clears in its own colours.
;               -> [dl_conansi].  All registers preserved.
;
; THROUGH THE REAL-MODE VECTOR TABLE, where the driver's INIT put its
; handler, so the question is asked in V86: INT 2Fh and a RETF planted
; in the scratch block and run as an excursion, on one of its levels'
; stacks, with DEVLOAD's call state kept round it as aspi_run keeps it
; - dl_load is in the middle of a driver and still needs all of it.
;---------------------------------------------------------------------
dl_conprobe:
        pushad
        push    dword ptr [dl_drvseg]
        push    dword ptr [dl_entry]
        push    dword ptr [dl_pktoff]
        push    dword ptr [dl_pktflat]
        push    dword ptr [dl_stktop]
        push    dword ptr [dl_hdrflat]
        movzx   ecx,byte ptr [dl_lost]
        push    ecx
        mov     byte ptr [dl_conansi],0
        call    dl_enter                ; -> EBX = the level's packet offset
        jc      dcp_back
        movzx   edi,word ptr [dl_seg]
        shl     edi,4
        mov     dword ptr [edi+DL_PROBE],00CB2FCDh      ; INT 2Fh / RETF
        mov     eax,[dl_seg]
        mov     [dl_drvseg],eax
        mov     dword ptr [dl_entry],DL_PROBE
        mov     [dl_pktoff],ebx
        lea     eax,[ebx+DL_STK]
        mov     [dl_stktop],eax
        mov     dword ptr [dl_inax],1A00h
        mov     byte ptr [dl_lost],0
        call    dl_excur
        call    dl_leave
        cmp     byte ptr [dl_lost],0
        jne     dcp_back
        cmp     byte ptr [dl_outax],0FFh
        jne     dcp_back
        mov     byte ptr [dl_conansi],1
dcp_back:
        pop     ecx
        mov     byte ptr [dl_lost],cl
        pop     dword ptr [dl_hdrflat]
        pop     dword ptr [dl_stktop]
        pop     dword ptr [dl_pktflat]
        pop     dword ptr [dl_pktoff]
        pop     dword ptr [dl_entry]
        pop     dword ptr [dl_drvseg]
        popad
        ret

;---------------------------------------------------------------------
; dl_write - ECX bytes at ESI to loaded device row EAX, in as few WRITE
;            requests as DL_XBUF holds.  No error return, as dl_out has
;            none: the driver's error goes to INT 24h, and Retry sends
;            the same piece again.  All registers preserved.
;
; A REQUEST FOR A RUN, NOT FOR A BYTE.  Each request is a trip into V86
; and back, and a console driver is spoken to for everything on the
; screen; a line DIR writes with 40h is one request, not eighty.  The
; source may be anywhere - a native program's buffer is above the
; megabyte - so the bytes go by way of DL_XBUF, as dl_ioctl's do.
;---------------------------------------------------------------------
dl_write:
        pushad
        mov     edx,eax                 ; the row
        mov     ebp,ecx                 ; what is left
dlw_piece:
        or      ebp,ebp
        jz      dlw_out
        mov     ecx,ebp
        cmp     ecx,DL_XBUFLEN
        jbe     dlw_size
        mov     ecx,DL_XBUFLEN
dlw_size:
        push    ecx                     ; this piece
dlw_try:
        mov     ecx,[esp]
        push    esi
        movzx   edi,word ptr [dl_seg]
        shl     edi,4
        add     edi,DL_XBUF
        cld
        rep     movsb
        pop     esi
        mov     eax,edx
        mov     ecx,DEVC_WRITE
        call    dl_prep                 ; -> EDI = the packet
        jc      dlw_drop
        mov     word ptr [edi+REQC_BUFOFF],DL_XBUF
        mov     eax,[dl_seg]
        mov     word ptr [edi+REQC_BUFSEG],ax
        mov     eax,[esp]
        mov     word ptr [edi+REQC_COUNT],ax
        call    dl_fire
        jnc     dlw_next
        call    dl_crit                 ; -> AL = the answer
        cmp     al,CERR_RETRY
        je      dlw_try
dlw_drop:
        pop     ecx                     ; Ignore or Fail: the rest goes
        jmp     short dlw_out           ; nowhere, as a byte to dl_out does
dlw_next:
        pop     ecx
        add     esi,ecx
        sub     ebp,ecx
        jmp     short dlw_piece
dlw_out:
        popad
        ret

;---------------------------------------------------------------------
; dl_peek - EAX = a loaded driver's row.  Its non-destructive read: CF
;           clear and AL = the byte the next read will give, CF set if
;           there is none (the busy bit) or the driver failed it.  Every
;           other register preserved.
;---------------------------------------------------------------------
dl_peek:
        push    ebx
        push    ecx
        push    edi
        mov     ecx,DEVC_PEEK
        call    dl_prep
        jc      dpk_none
        mov     byte ptr [edi+REQ_LEN],REQC_MEDIA + 1
        call    dl_fire                 ; -> AX = the status
        jc      dpk_none
        test    eax,DEVS_BUSY
        jnz     dpk_none
        mov     al,[edi+REQC_MEDIA]
        pop     edi
        pop     ecx
        pop     ebx
        clc
        ret
dpk_none:
        pop     edi
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; con_put    - AL to the console, as DOS's cooked output puts it there:
;              a TAB is spaces to the next eighth column, which DOS does
;              and not the driver.  All registers preserved.
; con_putraw - AL to the console as it is (06h, INT 29h, raw mode).
;---------------------------------------------------------------------
con_put:
        cmp     dword ptr [dl_conrow],-1
        jne     cp_drv
        jmp     dword ptr ds:[IOS_CONOUT]
cp_drv:
        cmp     al,9
        jne     con_putraw
        pushad
        call    con_tabsp               ; -> ECX = the spaces
        mov     esi,OFFSET dlc_spaces
        mov     eax,[dl_conrow]
        call    dl_write
        popad
        ret
con_putraw:
        cmp     dword ptr [dl_conrow],-1
        jne     cpr_drv
        jmp     dword ptr ds:[IOS_CONOUT]
cpr_drv:
        push    edx
        mov     dh,byte ptr [dl_conrow]
        call    dl_out
        pop     edx
        ret

;--- con_tabsp - ECX = the spaces from the cursor to the next tab stop,
;--- by the BIOS data area's column for the page on show: the driver
;--- keeps it, and nothing else knows where the cursor is
con_tabsp:
        push    eax
        movzx   eax,byte ptr ds:[BDA_VPAGE]
        and     eax,7
        movzx   eax,byte ptr ds:[BDA_CURPOS + eax*2]
        and     eax,7
        mov     ecx,8
        sub     ecx,eax
        pop     eax
        ret

;---------------------------------------------------------------------
; con_wrun - ECX bytes at ESI to the console as a cooked write: TABs as
;            spaces, the keyboard looked at every 64th character
;            (brk_out, which may break), and each byte to the printer
;            while ^P has the echo on.  Through the driver, a run at a
;            time.  All registers preserved.
;---------------------------------------------------------------------
con_wrun:
        pushad
cwr_lp:
        jecxz   cwr_out
        mov     edi,esi                 ; the run up to the next TAB
        xor     edx,edx
cwr_scan:
        cmp     edx,ecx
        jae     cwr_run
        cmp     byte ptr [edi+edx],9
        je      cwr_run
        inc     edx
        jmp     short cwr_scan
cwr_run:
        or      edx,edx
        jz      cwr_tab
        push    ecx
        mov     ecx,edx
        call    brk_out
        mov     eax,[dl_conrow]
        call    dl_write
        call    cwr_echo
        pop     ecx
        add     esi,edx
        sub     ecx,edx
        jmp     short cwr_lp
cwr_tab:
        call    brk_out1
        mov     al,9
        call    con_put
        call    pe_char
        inc     esi
        dec     ecx
        jmp     short cwr_lp
cwr_out:
        popad
        ret

;--- cwr_echo - the ECX bytes at ESI to the printer, if ^P has it on
cwr_echo:
        cmp     byte ptr [prn_echo],0
        je      cwe_out
        pushad
cwe_lp:
        mov     al,[esi]
        call    pe_char
        inc     esi
        loop    cwe_lp
        popad
cwe_out:
        ret

;---------------------------------------------------------------------
; con_kst - ZF set if the console has no key to give; ZF clear and AL
;           = the key's first byte if it has.  EAX may change, nothing
;           else does.
; con_krd - the next key -> AX, AL its character and AH its scan code
;           (0 from a driver) - IOS_CONIN's answer, and as with it a key
;           with no character is AL = 0 and the scan code in AH: a
;           driver gives those as two bytes and both are read.
;
; THE BREAK KEY IS THE KERNEL'S, driver or none: DOS's INT 1Bh mark
; (kb_break) comes before anything a driver has to give, and IOS_CONIN
; takes it, as MS-DOS's CON took its own.
;
; Under PM-DOS a driver is read only once it has said a key is there,
; because nothing in V86 may wait for one - see ANSI.ASM.
;---------------------------------------------------------------------
con_kst:
        cmp     byte ptr [dl_conin],0
        je      cks_kbd
        cmp     byte ptr ds:[DOSDATA + GNLS_BRKFLAG],0
        jne     cks_kbd
        mov     eax,[dl_conrow]
        call    dl_peek
        jc      cks_none
        or      esp,esp                 ; ZF clear
        ret
cks_none:
        cmp     eax,eax                 ; ZF set
        ret
cks_kbd:
        jmp     dword ptr ds:[IOS_CONSTAT]

con_krd:
        cmp     byte ptr [dl_conin],0
        je      ckr_kbd
        push    edx
ckr_wait:
        cmp     byte ptr ds:[DOSDATA + GNLS_BRKFLAG],0
        jne     ckr_brk                 ; the mark, even one made while
        call    con_kst                 ; this waited
        jnz     ckr_read
        call    dos_idle
        jmp     short ckr_wait
ckr_brk:
        pop     edx
        jmp     dword ptr ds:[IOS_CONIN]
ckr_read:
        mov     dh,byte ptr [dl_conrow]
        call    dl_in                   ; -> AL, AH = 0
        jc      ckr_none
        or      al,al
        jnz     ckr_out
        push    eax
        call    dl_in                   ; the scan code
        mov     dl,al
        pop     eax
        jc      ckr_out
        mov     ah,dl
ckr_out:
        pop     edx
        ret
ckr_none:
        xor     eax,eax
        pop     edx
        ret
ckr_kbd:
        jmp     dword ptr ds:[IOS_CONIN]

;--- con_kflush - nothing left to read: the driver's own flush, which
;--- empties what it owes and the BIOS's buffer both
con_kflush:
        cmp     byte ptr [dl_conin],0
        je      ckf_kbd
        pushad
        mov     edx,[dl_conrow]
        shl     edx,8                   ; the id dl_short wants: row * 256
        add     edx,DEV_LOADED
        mov     ecx,DEVC_INFLUSH
        call    dl_short
        popad
        ret
ckf_kbd:
        push    eax
ckf_lp:
        call    dword ptr ds:[IOS_CONSTAT]
        jz      ckf_out
        call    dword ptr ds:[IOS_CONIN]
        jmp     short ckf_lp
ckf_out:
        pop     eax
        ret

;--- con_cls - the screen cleared for 21h/F000h: by ESC[2J when the
;--- console is an ANSI.SYS, so that it clears in ANSI.SYS's colours as
;--- MS-DOS's CLS does, and by the kernel's console otherwise
con_cls:
        cmp     byte ptr [dl_conansi],0
        je      ccl_kbd
        pushad
        mov     esi,OFFSET dlc_cls
        mov     ecx,4
        mov     eax,[dl_conrow]
        call    dl_write
        popad
        ret
ccl_kbd:
        jmp     dword ptr ds:[IOS_CLS]

;--- con_devid - EDX = a device id.  If it is the console and a driver is
;--- the console, EDX = that driver's id instead, for the calls that are
;--- the driver's to answer (44h's IOCTLs).  Everything else kept.
con_devid:
        cmp     dl,DEV_CON
        jne     cdi_out
        cmp     dword ptr [dl_conrow],-1
        je      cdi_out
        mov     edx,[dl_conrow]
        shl     edx,8
        add     edx,DEV_LOADED
cdi_out:
        ret

dlc_conname db  'CON     '
dlc_cls     db  1Bh,'[2J'
dlc_spaces  db  8 dup (' ')

;=====================================================================
; THE REST OF WHAT DOS SENDS A CHARACTER DRIVER (2026-10-06): IOCTL
; read and write, the generic IOCTL, the two status requests, and open
; and close.  Until now a loaded driver could be read and written a
; byte at a time and asked nothing else, and 44h said so: "PM-DOS has
; no installable drivers to pass one to".  It has had them since v0.37.
; The call that made it matter is 4402h on IFS$HLP$, which Windows for
; Workgroups' file system manager makes of IFSHLP.SYS - under RMDOS,
; whose RMDEV.INC answers it there; this is the same thing for a DOS
; box and a native program.
;=====================================================================

;---------------------------------------------------------------------
; dl_rowhdr - EDX = a loaded device's id -> EBX = its header, flat, and
;             EAX = its row in dl_tab.  CF if there is no such row.
;---------------------------------------------------------------------
dl_rowhdr:
        mov     eax,edx
        shr     eax,8                   ; DEV_UNIT is 256: what is left is
        cmp     eax,[dl_count]          ; the row
        jae     drh_bad
        push    eax
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        movzx   ebx,word ptr [eax+DLE_SEG]
        shl     ebx,4
        movzx   eax,word ptr [eax+DLE_HDR]
        add     ebx,eax
        pop     eax
        clc
        ret
drh_bad:
        stc
        ret

;---------------------------------------------------------------------
; dl_errcode - AX = a status word with the error bit set -> EAX = the DOS
;          error for it: the driver's code, 0-0Fh, is 13h-22h, as
;          MS-DOS's SET_I24_EXTENDED_ERROR makes it.
;---------------------------------------------------------------------
dl_errcode:
        and     eax,0Fh
        add     eax,13h
        ret

;---------------------------------------------------------------------
; dl_ioctl - IOCTL read or write.  EDX = the device id, ECX = DEVC_IOCTLIN
;            or DEVC_IOCTLOUT, EDI = the caller's buffer, flat, EAX =
;            the count.  -> EAX = bytes moved; CF and EAX = the error.
;
; BELOW THE MEGABYTE the driver is handed the buffer where it is, as a
; segment and an offset: a DOS box's, and the only kind it has.  ABOVE
; it - a native program's - the bytes go through DL_XBUF in the scratch
; block, and an IOCTL longer than that is refused: these are control
; blocks, a few bytes to a few hundred, and one cannot be split in two.
;---------------------------------------------------------------------
dl_ioctl:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     [dli_cmd],ecx
        mov     [dli_buf],edi
        mov     [dli_cnt],eax
        mov     byte ptr [dli_bounce],0
        lea     esi,[edi+eax]
        cmp     esi,100000h
        jbe     dli_low
        cmp     eax,DL_XBUFLEN
        ja      dli_func
        mov     byte ptr [dli_bounce],1
dli_low:
        call    dl_rowhdr               ; -> EAX = the row
        jc      dli_func
        test    word ptr [ebx+DEVH_ATTR],DEV_ATTR_IOCTL
        jz      dli_func                ; the header says it takes none
        mov     ecx,[dli_cmd]
        call    dl_prep                 ; -> EDI = the packet
        jc      dli_func
        mov     eax,[dli_cnt]
        mov     word ptr [edi+REQC_COUNT],ax
        cmp     byte ptr [dli_bounce],0
        jne     dli_viabuf
        mov     eax,[dli_buf]
        mov     edx,eax
        shr     edx,4
        and     eax,0Fh
        mov     word ptr [edi+REQC_BUFOFF],ax
        mov     word ptr [edi+REQC_BUFSEG],dx
        jmp     short dli_go
dli_viabuf:
        mov     word ptr [edi+REQC_BUFOFF],DL_XBUF
        mov     eax,[dl_seg]
        mov     word ptr [edi+REQC_BUFSEG],ax
        cmp     dword ptr [dli_cmd],DEVC_IOCTLOUT
        jne     dli_go
        push    edi                     ; out: the caller's bytes first
        mov     esi,[dli_buf]
        mov     edi,[dl_seg]
        shl     edi,4
        add     edi,DL_XBUF
        mov     ecx,[dli_cnt]
        cld
        rep     movsb
        pop     edi
dli_go:
        call    dl_fire                 ; -> AX, CF; EDI still the packet
        jc      dli_err
        movzx   eax,word ptr [edi+REQC_COUNT]
        cmp     byte ptr [dli_bounce],0
        je      dli_ok
        cmp     dword ptr [dli_cmd],DEVC_IOCTLIN
        jne     dli_ok
        push    eax                     ; in: the driver's bytes back
        mov     ecx,eax
        mov     esi,[dl_seg]
        shl     esi,4
        add     esi,DL_XBUF
        mov     edi,[dli_buf]
        cld
        rep     movsb
        pop     eax
dli_ok:
        clc
        jmp     short dli_out
dli_err:
        call    dl_errcode
        stc
        jmp     short dli_out
dli_func:
        mov     eax,ERR_FUNC
        stc
dli_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; dl_genioctl - the generic IOCTL for a character device, 440Ch.  EDX =
;               the device id, ECX = CH category and CL function, ESI
;               and EDI the caller's SI and DI, EAX = its parameter
;               block, flat.  -> CF and EAX = the error.
;
; The block's length belongs to the category and is not ours to know,
; so it is passed only from below the megabyte: a native program's is
; refused rather than half copied.
;---------------------------------------------------------------------
dl_genioctl:
        push    ebx
        push    ecx
        push    edx
        push    edi
        cmp     eax,100000h
        jae     dgi_func
        mov     [dli_buf],eax
        mov     [dli_cnt],ecx
        mov     [dli_cmd],esi           ; the caller's SI, kept a moment
        push    edi
        call    dl_rowhdr               ; -> EAX = the row, EBX = header
        pop     edi
        jc      dgi_func
        test    word ptr [ebx+DEVH_ATTR],DEV_ATTR_GENIOCTL
        jz      dgi_func
        push    edi                     ; the caller's DI
        mov     ecx,DEVC_GENIOCTL
        call    dl_prep                 ; -> EDI = the packet
        pop     edx
        jc      dgi_func
        mov     byte ptr [edi+REQ_LEN],REQG_LEN
        mov     eax,[dli_cnt]
        mov     byte ptr [edi+REQG_MINOR],al
        mov     byte ptr [edi+REQG_CAT],ah
        mov     eax,[dli_cmd]
        mov     word ptr [edi+REQG_SI],ax
        mov     word ptr [edi+REQG_DI],dx
        mov     eax,[dli_buf]
        mov     edx,eax
        shr     edx,4
        and     eax,0Fh
        mov     word ptr [edi+REQG_PKTOFF],ax
        mov     word ptr [edi+REQG_PKTSEG],dx
        call    dl_fire
        jnc     dgi_out
        call    dl_errcode
        stc
        jmp     short dgi_out
dgi_func:
        mov     eax,ERR_FUNC
        stc
dgi_out:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; dl_short - EDX = a loaded device's id, ECX = a request that is the
;            header alone: open, close, input or output status.
;            -> EAX = the status word; CF if the driver failed it, or
;            if there is no such device.
;---------------------------------------------------------------------
dl_short:
        push    ebx
        push    edi
        call    dl_rowhdr
        jc      dsh_none
        call    dl_prep
        jc      dsh_none
        mov     byte ptr [edi+REQ_LEN],REQ_HDRLEN
        call    dl_fire                 ; -> AX, CF
        movzx   eax,ax
        pop     edi
        pop     ebx
        ret
dsh_none:
        xor     eax,eax
        pop     edi
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; dl_devopen / dl_devclose - ESI = an SFT entry.  If it is a loaded
;            driver whose header asks to hear about opens and closes,
;            tell it.  dl_devopen: CF and EAX = the error if the driver
;            refused.  Both preserve everything else.
;
; ONE OPEN AND ONE CLOSE FOR EACH SLOT, and a DOS program's duplicate
; handles share a slot (JFT.INC), so the driver hears about each file
; it really has open.  A native program's 45h and 46h copy the slot,
; and send another open for the copy so that the two closes it will
; get are matched.
;---------------------------------------------------------------------
dl_devopen:
        push    ebx
        push    ecx
        push    edx
        push    eax                     ; the caller's - open_core's
        call    dl_ocrm                 ; handle is in it
        jc      dlo2_ok
        mov     ecx,DEVC_OPEN
        call    dl_short
        jnc     dlo2_ok
        call    dl_errcode
        add     esp,4                   ; the error replaces it
        stc
        jmp     short dlo2_out
dlo2_ok:
        pop     eax
        clc
dlo2_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

dl_devclose:
        pushad
        call    dl_ocrm
        jc      dlc2_out
        mov     ecx,DEVC_CLOSE
        call    dl_short                ; a refusal to close is not one
dlc2_out:                               ; anybody can act on
        popad
        ret

;--- dl_ocrm - ESI = an SFT entry -> EDX = its device id, CF clear if
;--- it is a loaded driver whose header has the open/close bit.
dl_ocrm:
        push    eax
        push    ebx
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jz      doc_no
        mov     edx,[esi+SFT_DEVID]
        cmp     dl,DEV_LOADED
        jne     doc_no
        call    dl_rowhdr
        jc      doc_no
        test    word ptr [ebx+DEVH_ATTR],DEV_ATTR_OCRM
        jz      doc_no
        pop     ebx
        pop     eax
        clc
        ret
doc_no:
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; dl_devinfo - EDX = a loaded device's id -> DX = the device information
;              word 4400h answers, as DOS builds it from the header:
;              the attribute word's high byte (IOCTL, output until
;              busy, open and close), "a device, input not at its end",
;              and the attribute's low five bits.  CF if no such row.
;---------------------------------------------------------------------
dl_devinfo:
        push    eax
        push    ebx
        call    dl_rowhdr
        jc      ddi_out
        movzx   edx,word ptr [ebx+DEVH_ATTR]
        and     edx,0FF1Fh
        or      dl,0C0h
        clc
ddi_out:
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; dl_unitfar - AL = a driver unit's pseudo BIOS number (DL_UNIT_BASE
;              and up) -> EAX = its driver's header as a far pointer,
;              the segment in the high word, and CL = the driver's own
;              number for the unit.  CF if it is not one.  For PARK.INC,
;              which hands the unit to RMDOS - and which is assembled
;              before the equates here, so it asks rather than looks.
;---------------------------------------------------------------------
dl_unitfar:
        push    edx
        movzx   eax,al
        sub     eax,DL_UNIT_BASE
        jb      duf_no
        cmp     eax,[dl_nunits]
        jae     duf_no
        imul    edx,eax,DLU_LEN
        add     edx,OFFSET dl_units
        cmp     byte ptr [edx+DLU_KIND],DLK_RM
        jne     duf_no                  ; a 32-bit driver's has no header
        mov     cl,[edx+DLU_UNIT]
        mov     eax,[edx+DLU_TAB]       ; the dl_tab row
        cmp     eax,[dl_count]
        jae     duf_no
        imul    eax,DLE_LEN
        add     eax,OFFSET dl_tab
        movzx   edx,word ptr [eax+DLE_SEG]
        shl     edx,16
        mov     dx,word ptr [eax+DLE_HDR]
        mov     eax,edx
        pop     edx
        clc
        ret
duf_no:
        pop     edx
        stc
        ret

;---------------------------------------------------------------------
; dl_unitmem - AL = a driver unit's pseudo BIOS number -> EAX = where
;              this kernel keeps its size and address, CF if it is not
;              a 32-bit driver's memory unit (NDUF_MEMORY).  For PARK.INC
;              too: RMDOS reads the two dwords there, DLU_SECS and then
;              DLU_MEM, and moves the unit's sectors itself while no
;              driver can be called.
;---------------------------------------------------------------------
dl_unitmem:
        movzx   eax,al
        sub     eax,DL_UNIT_BASE
        jb      dum_no
        cmp     eax,[dl_nunits]
        jae     dum_no
        imul    eax,DLU_LEN
        add     eax,OFFSET dl_units
        cmp     byte ptr [eax+DLU_KIND],DLK_D32
        jne     dum_no
        cmp     dword ptr [eax+DLU_MEM],0
        je      dum_no
        add     eax,DLU_SECS
        clc
        ret
dum_no:
        stc
        ret

dli_cmd     dd  0
dli_buf     dd  0
dli_cnt     dd  0
dli_bounce  db  0
            db  0,0,0

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
dl_then     dd  -1                      ; the interrupt entry strategy's RETF
                                        ; lands on, or -1 (dl_excur)
dl_inax     dd  0                       ; EAX for the next excursion
dl_outax    dd  0                       ; ...and as the last one left it
dl_conrow   dd  -1                      ; the console's dl_tab row, or -1
dl_conin    db  0                       ; 1: and it reads the keyboard
dl_conansi  db  0                       ; 1: and answers INT 2Fh AX=1A00h
            db  0,0
dl_sft      dd  0
dl_fsize    dd  0
dl_got      dd  0
dl_err      dd  0
dl_linelen  dd  0
dl_fnext    dd  0
dl_endoff   dd  0
dl_endseg   dd  0
dl_brkseg   dd  0                       ; the end of the room INIT was given
dlx_v86fn   db  25h, 35h, 34h, 50h, 51h, 62h, 2Fh, 52h, 38h, 65h
.ERRNZ  ($ - dlx_v86fn) - DLX_NV86, <DLX_NV86 is not the length of dlx_v86fn>
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
dl_ndrec    dd  0                       ; a 32-bit disk driver's records,
dl_ndrow    dd  0                       ; its nd_tab row, and how many of
dl_ndgot    dd  0                       ; its units got a letter
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
; the filesystem hands down are sec_buf and clus_buf, the kernel's own
; and above 1MB, which no segment:offset can name.  So every transfer is
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
        mov     byte ptr [edi+DLU_KIND],DLK_RM
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
        jmp     dbi_unit
dbi_done:
        popad
        clc
        ret
dbi_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dl_unitroom - ECX = how many more driver units can have a letter, and
;               EBX = the letter the next one gets (0 = A:).  The fewer
;               of the rows left in dl_units and the letters left in
;               drv_tbl.  What a 32-bit disk driver is told at INIT
;               (NDLOAD.INC), so that it can refuse before it has taken
;               memory for a unit nobody could reach.
;---------------------------------------------------------------------
dl_unitroom:
        push    eax
        mov     ecx,DLU_MAX
        sub     ecx,[dl_nunits]
        movzx   ebx,byte ptr [drv_count]
        mov     eax,MAX_DRIVES
        sub     eax,ebx
        jae     dur_some
        xor     eax,eax
dur_some:
        cmp     ecx,eax
        jbe     dur_out
        mov     ecx,eax
dur_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; dl_ndunits - a 32-bit disk driver's INIT has come back CF clear
;              (NDLOAD.INC).  ESI -> its row in nd_tab.  Give each unit
;              its table names a drive letter.  CF if none got one.
;
; WHAT THE DRIVER WROTE IS CHECKED BEFORE A LETTER POINTS AT IT.  The
; records have to be inside the driver's own block, each transfer
; routine inside it too, and a memory unit's sectors have to end before
; the address space does.  A unit that fails is left without a letter
; and its NDU_LETTER says FFh; the ones beside it are still given one.
;
; THE SIZE IS THE DRIVER'S, in DRV_SECS, so that fat_mount believes a
; boot sector only as far as the unit really reaches (fat_partsecs) -
; the same thing a partition's length does for a hard disk.
;
; AND A MEMORY UNIT IS NOT CACHED: its cache_drvst row is cleared, the
; same thing INT 2Fh 4A10h BX=3 does when told to stop caching a drive.
; The kernel's cache would hold a second copy of sectors that are
; already memory and spend the time copying them there.
;---------------------------------------------------------------------
dl_ndunits:
        pushad
        mov     ebx,[esi+ND_TABLE]
        mov     ecx,[ebx+NDT_NUNITS]
        or      ecx,ecx
        jz      dnu_none
        cmp     ecx,DLU_MAX
        ja      dnu_none
        mov     [dl_bunits],ecx
        mov     edi,[ebx+NDT_UNITS]
        mov     [dl_ndrec],edi
        mov     eax,edi
        sub     eax,[esi+ND_BASE]
        jb      dnu_none
.ERRNZ  NDU_LEN - 16, <dl_ndunits multiplies by NDU_LEN with a shift>
        shl     ecx,4
        add     eax,ecx
        jc      dnu_none
        cmp     eax,[esi+ND_SIZE]
        ja      dnu_none

        mov     eax,esi                 ; which nd_tab row, for DLU_TAB
        sub     eax,OFFSET nd_tab
        xor     edx,edx
        mov     ecx,ND_LEN
        div     ecx
        mov     [dl_ndrow],eax
        mov     dword ptr [dl_ndgot],0

        xor     ebp,ebp                 ; the unit, the driver's number
dnu_unit:
        cmp     ebp,[dl_bunits]
        jae     dnu_done
        cmp     dword ptr [dl_nunits],DLU_MAX
        jae     dnu_done
        mov     edi,ebp
        shl     edi,4
        add     edi,[dl_ndrec]          ; EDI -> this unit's record
        mov     byte ptr [edi+NDU_LETTER],0FFh

        mov     eax,[edi+NDU_IO]        ; its transfer, inside the block
        sub     eax,[esi+ND_BASE]
        jb      dnu_next
        cmp     eax,[esi+ND_SIZE]
        jae     dnu_next
        mov     ecx,[edi+NDU_SECS]      ; and something to transfer
        or      ecx,ecx
        jz      dnu_next
        xor     edx,edx                 ; EDX = DLU_MEM
        test    byte ptr [edi+NDU_FLAGS],NDUF_MEMORY
        jz      dnu_row
        cmp     ecx,800000h
        jae     dnu_next                ; 4GB of sectors, or more
        mov     edx,ecx
        shl     edx,9
        add     edx,[edi+NDU_MEM]
        jc      dnu_next                ; past the top of the address space
        mov     edx,[edi+NDU_MEM]
        or      edx,edx
        jz      dnu_next                ; and memory at nought is not memory

dnu_row:
        mov     eax,[dl_nunits]
        imul    eax,DLU_LEN
        add     eax,OFFSET dl_units     ; EAX -> the dl_units row
        mov     [eax+DLU_SECS],ecx
        mov     [eax+DLU_MEM],edx
        mov     edx,[dl_ndrow]
        mov     [eax+DLU_TAB],edx
        mov     edx,ebp
        mov     [eax+DLU_UNIT],dl
        mov     dl,[edi+NDU_MEDIA]
        mov     [eax+DLU_MEDIA],dl
        mov     byte ptr [eax+DLU_KIND],DLK_D32
        mov     edx,[edi+NDU_IO]
        mov     [eax+DLU_IO],edx
        mov     [eax+DLU_REC],edi

        push    ebx                     ; and a letter, ECX sectors long
        mov     eax,[dl_nunits]
        add     eax,DL_UNIT_BASE        ; AL = the pseudo number, AH = no
        xor     edx,edx                 ; partition type
        mov     bl,0FFh
        call    drv_add
        pop     ebx
        jc      dnu_done                ; out of letters
        movzx   eax,byte ptr [drv_count]        ; the row it just took
        dec     eax
        mov     [edi+NDU_LETTER],al
        imul    eax,DRV_LEN
        add     eax,OFFSET drv_tbl
        or      byte ptr [eax+DRV_FLAGS],DRVF_DRIVER

        test    byte ptr [edi+NDU_FLAGS],NDUF_MEMORY
        jz      dnu_counted
        push    ebx
        mov     eax,[dl_nunits]
        add     eax,DL_UNIT_BASE
        call    cache_unitof            ; -> EBX = its cache_drvst row
        jc      dnu_nocache
        mov     byte ptr [cache_drvst+ebx],0
dnu_nocache:
        pop     ebx
dnu_counted:
        inc     dword ptr [dl_nunits]
        inc     dword ptr [dl_ndgot]
dnu_next:
        inc     ebp
        jmp     dnu_unit
dnu_done:
        cmp     dword ptr [dl_ndgot],0
        je      dnu_none
        popad
        clc
        ret
dnu_none:
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
        movzx   eax,al
        sub     eax,DL_UNIT_BASE
        cmp     eax,[dl_nunits]
        jae     dbio_fail
        imul    eax,DLU_LEN
        add     eax,OFFSET dl_units
        mov     [dl_iorow],eax
        cmp     byte ptr [eax+DLU_KIND],DLK_D32
        je      dbio_d32

        call    dl_stage                ; a real-mode driver's, so the
        jc      dbio_fail               ; buffer it can reach

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

; A 32-BIT DRIVER'S UNIT IS CALLED, NOT SENT PACKETS.  It runs where
; the kernel does, so the whole transfer goes in one call, straight
; between its sectors and the caller's buffer - no staging, no chunks,
; and no excursion.  The sectors are checked against the unit's size
; here, so that a driver is never asked for one it does not have, and
; the status it answers is the one the caller sees: the pushad frame's
; AH, which the popad would otherwise put back as it came in.
dbio_d32:
        mov     esi,eax                 ; ESI -> the row
        movzx   ecx,cx
        jecxz   dbio_done
        mov     eax,ebx
        add     eax,ecx
        jc      dbio_nosec
        cmp     eax,[esi+DLU_SECS]
        ja      dbio_nosec
        mov     al,[dl_iowr]
        mov     edx,[esi+DLU_REC]
        cld
        call    dword ptr [esi+DLU_IO]
        jc      dbio_status
        popad
        clc
        ret
dbio_nosec:
        mov     ah,04h                  ; sector not found
dbio_status:
        mov     byte ptr [esp+29],ah    ; the frame's EAX is at ESP+28
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

