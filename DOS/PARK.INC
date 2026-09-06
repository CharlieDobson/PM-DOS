;=====================================================================
; PARK.INC - PM-DOS's side of the handover
;
; Loads RMDOS.SYS into the DOS box's arena, fills in its handoff block,
; leaves protected mode for good, and picks the machine back up when
; the real-mode side gives it back.
;
;---------------------------------------------------------------------
; WHAT IS AND IS NOT SAVED.
;
; Almost nothing, and that is the design rather than an omission.
; PM-DOS's own state lives above 1MB and is not touched by anything
; that runs while we are parked: RMDOS is in conventional memory, the
; program it runs is in conventional memory, and the arena they share
; is the DOS box's arena, which no 32-bit structure points into.
;
; The machine state that DOES change is all machine state:
;
;   the IVT       RMDOS installs a DOS on 20h-2Fh.  PM-DOS's guest
;                 stubs go back afterwards - see vdm_stubs.
;   the PICs      rm_park moves them to the BIOS layout on the way
;                 down and rm_unpark moves them back on the way up.
;   A20           forced on by rm_unpark, because whatever ran while
;                 we were away owned the gate and the kernel this
;                 returns to lives above the megabyte.
;   the disks     RMDOS writes to them, so every cached cluster and
;                 FAT window up here is now a guess.  Every volume is
;                 remounted on the way back.
;
; THE ONE RULE FOR THE CALLER: no file may be open when this is
; called.  RMDOS has its own handle table and its own idea of where
; each file is; two DOSes with a file open between them is a
; coherency problem with no owner.  It is checked below rather than
; documented and hoped for.
;=====================================================================

PARK_SLACK      EQU     256             ; paragraphs of working room past
                                        ; the image - RMDOS's buffers and
                                        ; stacks are inside the image, so
                                        ; this is only a margin

;=====================================================================
; park_run - ESI -> a flat ASCIIZ path of a 16-bit program to run
;                   under RMDOS, or 0 to resume the frame in
;                   [park_iret] instead, which is the Windows case.
;
; Returns EAX = the exit code, CF set if the excursion never happened.
;
; THE TWO WAYS IN ARE NOT SYMMETRIC AND IT IS WORTH SAYING WHY.  With a
; path, PM-DOS creates the process: it allocates the memory, builds the
; PSP, loads the image and tells RMDOS where to start it.  With a
; frame, THE PROCESS ALREADY EXISTS AND IS MID-INSTRUCTION - it is a
; guest that executed INT 2Fh AX=1605h and is owed a return from it -
; so there is nothing to create and the only thing to carry across is
; the register state.  park_fromv86 fills [park_iret] from the guest
; context and [park_psp] from the guest's current process; RMDOS's
; rd_resume turns that back into an IRET.
;
; What the guest cannot be told, and does not need to be, is that the
; IRET happens in a different processor mode from the INT.
;=====================================================================
park_run:
        push    ebx
        push    ecx
        push    edx
        push    edi
        push    ebp
        mov     dword ptr [park_prog],esi
        or      esi,esi
        jz      pk_keeppsp
        mov     word ptr [park_psp],0   ; a path means PM-DOS makes the
                                        ; process; anything park_fromv86
                                        ; left behind is last time's
pk_keeppsp:

        ; THE ONE HOLE IN THE IDENTITY MAP IS THE ONE WE ARE MOVING
        ; INTO.  PM-DOS emulates the A20 gate in the page tables -
        ; sixteen entries covering 100000h, pointing at themselves
        ; when the gate is open and at 00000h when it is shut - and
        ; the kernel is not exempt from its own page tables.  A DOS
        ; box starts with the gate SHUT, because that is what a real
        ; machine does, so a kernel write to linear 100000h lands on
        ; physical zero: on the interrupt vector table.  Loading RMDOS
        ; into the HMA that way overwrites the bottom of memory and
        ; then hands control to a segment FFFFh that has nothing in
        ; it, which is a triple fault two instructions later.
        ;
        ; So the alias goes to the identity for the length of the
        ; excursion and the guest's own idea of the gate is put back
        ; afterwards by park_release.  The REAL gate is already open
        ; and has been since boot - the kernel lives above the
        ; megabyte - and rm_unpark forces it open again on the way up.
        call    dword ptr ds:[IOS_A20GET]
        mov     byte ptr [park_a20],al
        mov     al,1
        call    dword ptr ds:[IOS_A20]

        ; No file may be open.  Handles 0 to 4 are the standard ones
        ; and are devices; anything above them is a file, and a file is
        ; a reason to refuse.
        call    park_filesopen
        jc      pk_busy

        call    park_loadrm             ; -> [park_seg]
        jc      pk_noimage

        cmp     dword ptr [park_prog],0
        je      pk_nofill
        call    park_loadprog           ; -> [park_pspseg]
        jc      pk_nomem
pk_nofill:

        call    park_xmsreclaim         ; what the parking program is holding
        call    park_xmsgrab            ; the extended memory to hand over
        call    park_chaincut           ; ...and the memory that is about to
                                        ; stop existing
        call    park_fillhdr
        call    park_go

        ; And we are back.
        call    park_recover
        movzx   eax,word ptr [park_exit]
        clc
        jmp     short pk_out

pk_busy:
        mov     eax,ERR_ACCESS
        stc
        jmp     short pk_out
pk_noimage:
        mov     eax,dword ptr [park_lderr]
        stc
        jmp     short pk_out
pk_nomem:
        mov     eax,ERR_NOMEM
        stc
pk_out:
        pushfd
        call    park_release
        popfd
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; park_fromv86 - fill [park_iret] and [park_psp] from the DOS box that
;                is currently trapped in the monitor.
;
; Called before park_run(0).  The context is vdm_ctx, which the V86
; fault handler has already unpacked, and whose EIP has already been
; stepped past the two bytes of the INT that got us here - so this
; frame resumes the guest AT THE INSTRUCTION AFTER THE BROADCAST,
; which is exactly what a returning INT would have done.
;
; SS:SP IS COPIED UNCHANGED AND NOTHING IS PUSHED HERE.  In V86 mode
; with IOPL 0 the INT never reached the CPU's own interrupt path: it
; faulted first, and the three words an INT would have pushed were
; never pushed.  The guest's stack pointer is therefore still exactly
; where the guest left it, and rd_resume is free to build its IRET
; frame in the six dead bytes below it.
;=====================================================================
park_fromv86:
        pushad
        mov     edi,OFFSET park_iret

        ; NOT ZEROED FIRST, and not because it does not need to be:
        ; every one of the 48 bytes below is written, with no gaps -
        ; eight dwords of general registers and eight words of segments
        ; and flags is exactly RMI_LEN.  The clear that used to be here
        ; was a REP STOSD, which writes through ES, and see the loop in
        ; park_fillhdr for what that turned out to be worth.
        mov     eax,dword ptr [vdm_ctx+V86C_EAX]
        mov     dword ptr [edi + RMI_EAX - RMH_IRET],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EBX]
        mov     dword ptr [edi + RMI_EBX - RMH_IRET],eax
        mov     eax,dword ptr [vdm_ctx+V86C_ECX]
        mov     dword ptr [edi + RMI_ECX - RMH_IRET],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EDX]
        mov     dword ptr [edi + RMI_EDX - RMH_IRET],eax
        mov     eax,dword ptr [vdm_ctx+V86C_ESI]
        mov     dword ptr [edi + RMI_ESI - RMH_IRET],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EDI]
        mov     dword ptr [edi + RMI_EDI - RMH_IRET],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EBP]
        mov     dword ptr [edi + RMI_EBP - RMH_IRET],eax
        mov     eax,dword ptr [vdm_ctx+V86C_ESP]
        mov     dword ptr [edi + RMI_ESP - RMH_IRET],eax

        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        mov     word ptr [edi + RMI_CS - RMH_IRET],ax
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        mov     word ptr [edi + RMI_IP - RMH_IRET],ax
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        mov     word ptr [edi + RMI_SS - RMH_IRET],ax
        mov     eax,dword ptr [vdm_ctx+V86C_DS]
        mov     word ptr [edi + RMI_DS - RMH_IRET],ax
        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        mov     word ptr [edi + RMI_ES - RMH_IRET],ax
        mov     eax,dword ptr [vdm_ctx+V86C_FS]
        mov     word ptr [edi + RMI_FS - RMH_IRET],ax
        mov     eax,dword ptr [vdm_ctx+V86C_GS]
        mov     word ptr [edi + RMI_GS - RMH_IRET],ax

        ; THE FLAGS ARE THE GUEST'S, WITH THE MONITOR'S OWN BITS TAKEN
        ; OUT.  V86C_FLAGS carries VM and IOPL because that is how the
        ; guest was being run; on the other side there is no monitor,
        ; no V86 mode and no privilege level, and an IRET that put
        ; those bits back into a real-mode FLAGS register would be
        ; setting NT and IOPL in a machine that has no use for either.
        mov     eax,dword ptr [vdm_ctx+V86C_FLAGS]
        and     eax,0FFFh
        or      eax,EFL_IF                      ; it is about to run: let it
        mov     word ptr [edi + RMI_FLAGS - RMH_IRET],ax

        mov     word ptr [edi + RMI_GS - RMH_IRET],1605h ; a signature: this ran
        movzx   eax,word ptr ds:[vdm_curpsp]
        mov     word ptr [park_psp],ax
        popad
        ret

;---------------------------------------------------------------------
; park_filesopen - CF set if this process has a file open.
;---------------------------------------------------------------------
park_filesopen:
        push    eax
        push    ebx
        mov     ebx,5                   ; the standard five are devices
pfo_loop:
        cmp     ebx,20
        jae     pfo_clear
        push    ebx
        call    sft_get                 ; -> ESI, CF if not open
        pop     ebx
        jnc     pfo_busy
        inc     ebx
        jmp     short pfo_loop
pfo_clear:
        pop     ebx
        pop     eax
        clc
        ret
pfo_busy:
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; park_loadrm - find RMDOS.SYS, allocate low memory for it, read it in.
;---------------------------------------------------------------------
park_loadrm:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     word ptr [park_seg],0
        mov     word ptr [park_pspseg],0

        ; Open it.  It lives in the root of the boot volume, next to
        ; PMIO.SYS and PMDOS.SYS, and is found the same way they are.
        mov     dword ptr [park_lderr],ERR_NOFILE
        mov     edx,OFFSET park_name
        mov     eax,3D00h
        call    k21
        jc      plr_nofile
        mov     word ptr [park_hnd],ax
        mov     dword ptr [park_lderr],ERR_BADDATA

        ; How big?  Seek to the end and back.
        movzx   ebx,word ptr [park_hnd]
        mov     eax,4202h
        xor     ecx,ecx
        xor     edx,edx
        call    k21
        jc      plr_bad
        mov     dword ptr [park_size],eax
        cmp     eax,RM_ORG + 100h
        jbe     plr_bad                 ; nothing this short is an image
        cmp     eax,64 * 1024
        jae     plr_bad                 ; it has to fit in one segment, and
                                        ; everything in it assumes so

        ; BACK TO RM_ORG, NOT TO ZERO.  The file is a picture of the
        ; whole segment and its first RM_ORG bytes are the pad wlink
        ; put in front of the ORG - they are not part of the image,
        ; and in the HMA the address they belong to is in the ROM.
        ;
        ; THE OFFSET GOES IN ECX.  k21's 42h is not the 16-bit call's
        ; CX:DX - it takes one flat 32-bit offset in ECX and does not
        ; look at EDX at all.  Every 42h in this routine until now
        ; asked for zero and zeroed both registers, so which one
        ; carried the number had never once mattered; the first call
        ; that wanted a non-zero position put it in the wrong one and
        ; got a silent, successful seek to the beginning.
        movzx   ebx,word ptr [park_hnd]
        mov     eax,4200h
        mov     ecx,RM_ORG
        xor     edx,edx
        call    k21
        jc      plr_bad

        ; WHERE IT GOES, AND THE HMA FIRST.
        ;
        ; The arena is the obvious place and it is the wrong one.  A
        ; .COM owns the whole transient program area until it shrinks
        ; itself, and the caller that matters here - DOSX.EXE - does
        ; its 4Ah AFTER broadcasting 1605h rather than before, so at
        ; the instant it asks for the handover there is nothing free
        ; to put a DOS in.  v0.49 failed here with "not enough memory"
        ; on a machine with 620K of it.
        ;
        ; 100000h-10FFEFh is real memory with paging off, it is the
        ; only extended memory a 16-bit segment can reach, PM-DOS has
        ; kept it empty since v0.38, and it costs the DOS box nothing.
        ; It is where a real DOS puts itself.  Segment FFFFh, offset
        ; RM_ORG; see the note above RM_ORG in RMDOS.INC.
        ;
        ; AND YET IT GOES LOW FIRST NOW, from v0.53, because of what
        ; the Windows kernel does with the file table.  KRNL386 never
        ; asks where the SFT is.  It opens CON five times to plant
        ; handles, then scans memory for the name "CON " and takes the
        ; distance between two of them as the size of an entry - and
        ; the scan does not reach the megabyte.  Parked up here this
        ; DOS is invisible to it and the answer is "Incorrect MS-DOS
        ; version"; parked low the same build walks straight past.
        ;
        ; A real DOS with DOS=HIGH puts its CODE in the HMA and leaves
        ; the SFT below, which is the right shape and is eighty-one
        ; access sites away from here.  Until then the whole image
        ; goes where the table has to be, and the HMA becomes the
        ; fallback for when there is no room low - everything except
        ; Windows works from up there, and the reasoning above is
        ; still why it is worth keeping.
        mov     byte ptr [park_high],0
IFDEF RMDOS_HIGH
        jmp     short plr_hma           ; BUILD rmhigh - what the default
                                        ; was up to v0.52, kept reachable
                                        ; so the two can be compared one
                                        ; build apart instead of one
                                        ; release apart
ENDIF

        ; park_size is the file's size and the file is a picture of the
        ; segment from offset 0, so it already counts the RM_ORG bytes
        ; the image does not use - the arithmetic is unchanged.
plr_low:
        ; ONE WHOLE SEGMENT, ALWAYS, WHATEVER THE IMAGE WEIGHS.
        ;
        ; This used to ask for the image rounded up plus PARK_SLACK,
        ; which is the honest arithmetic and is the wrong answer.  It
        ; makes the address of everything ABOVE RMDOS a function of how
        ; big RMDOS happens to be, and Windows is sensitive to that to
        ; a degree this project has now been bitten by twice: the trace
        ; build stalls at the splash where the release build reaches
        ; the desktop, and the only difference between a run that
        ; reached Program Manager and one that did not was FORTY-EIGHT
        ; MORE BYTES of RMDOS.  An instrument that moves the machine it
        ; is measuring is not an instrument.
        ;
        ; So the block is a fixed 64K.  RMDOS already has to fit in one
        ; segment - everything in it assumes so, and park_loadrm
        ; refuses an image that does not - and owning the whole segment
        ; costs about forty kilobytes of a machine that has six hundred.
        ; What it buys is that park_seg, and therefore every address
        ; Windows will ever see, is the SAME in the release build and
        ; in the build with eight kilobytes of trace buffer in it.  A
        ; debug build and a release build can now be compared.
        mov     eax,dword ptr [park_size]
        add     eax,15
        shr     eax,4
        add     eax,PARK_SLACK
        cmp     eax,1000h
        ja      plr_bad                 ; too big to sit in one segment,
                                        ; which everything here assumes
        mov     eax,1000h
        mov     ebx,eax
        mov     dword ptr [park_lderr],ERR_NOMEM
        mov     word ptr [park_paras],ax        ; BEFORE the call: lm_alloc
                                        ; does not preserve EBX, and on
                                        ; failure it deliberately puts
                                        ; the largest free run there
        ; FIRST FIT, and deliberately.  Parking at the TOP of conventional
        ; memory was tried and is wrong: the Windows kernel scans for the
        ; SFT only as far as 80000h, and RMDOS at 96C2 - 626K - is past
        ; it, so the measurement fails exactly as it does from the HMA.
        ; And the reason it was tried does not hold either.  The kernel is
        ; not short of memory: KRNL386.EXE asks for MAXALLOC 0000, so 73K
        ; is image-plus-nothing and is the RIGHT answer.  Low it stays.
        call    lm_alloc                ; -> AX = segment
IFDEF RMDOS_LOW
        jc      plr_bad                 ; BUILD rmlow - low or nothing, so
                                        ; that a failure here is loud
ELSE
        jc      plr_hma
ENDIF
        mov     word ptr [park_seg],ax
        jmp     short plr_placed

plr_hma:
        mov     eax,dword ptr [park_size]
        add     eax,256                 ; and a little room to grow into
        cmp     eax,10000h
        ja      plr_bad
        mov     word ptr [park_seg],RM_HMA_SEG
        mov     word ptr [park_paras],1000h     ; the whole 64K, which is
                                        ; only read to work out SP
        mov     byte ptr [park_high],1

plr_placed:

        ; Read it straight into memory.  The address is flat here
        ; because the kernel is flat; it is the same bytes the
        ; real-mode side will address as [park_seg]:RM_ORG, and for the
        ; HMA that arithmetic - FFFF0h + 10h - is exactly the
        ; megabyte.
        movzx   edx,word ptr [park_seg]
        shl     edx,4
        add     edx,RM_ORG
        movzx   ebx,word ptr [park_hnd]
        mov     ecx,dword ptr [park_size]
        sub     ecx,RM_ORG
        push    ecx
        mov     eax,3F00h
        call    k21
        pop     ecx
        jc      plr_bad
        cmp     eax,ecx
        jne     plr_bad
IFDEF RMDOS_BISECT
        push    eax
        mov     eax,edx                         ; where we told it to go
        mov     edi,0B8000h + 160 + 100
        call    park_mhex
        mov     eax,dword ptr ds:[edx]          ; and what is there NOW -
        mov     edi,0B8000h + 160 + 120         ; before anything else has
        call    park_mhex                       ; had a chance to touch it
        pop     eax
ENDIF

        movzx   ebx,word ptr [park_hnd]
        mov     eax,3E00h
        call    k21
        mov     word ptr [park_hnd],0
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
plr_bad:
        movzx   ebx,word ptr [park_hnd]
        mov     eax,3E00h
        call    k21
        mov     word ptr [park_hnd],0
plr_nofile:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; park_loadprog - load the 16-bit .COM named at [park_prog] into the
;                 arena, with a PSP in front of it.
;
; Only .COM, and deliberately: this path exists to TEST RMDOS, and the
; program it runs is one of ours.  Loading an .EXE is RMDOS's own job
; through its 4Bh, which is where the relocation code belongs.
;---------------------------------------------------------------------
park_loadprog:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     esi,dword ptr [park_prog]
        mov     edx,esi
        mov     eax,3D00h
        call    k21
        jc      plp_bad2
        mov     word ptr [park_hnd],ax

        movzx   ebx,word ptr [park_hnd]
        mov     eax,4202h
        xor     ecx,ecx
        xor     edx,edx
        call    k21
        jc      plp_bad
        mov     dword ptr [park_psize],eax
        cmp     eax,0FE00h
        jae     plp_bad

        movzx   ebx,word ptr [park_hnd]
        mov     eax,4200h
        xor     ecx,ecx
        xor     edx,edx
        call    k21
        jc      plp_bad

        ; A PSP plus the image plus a stack.  4K of stack is generous
        ; for a .COM and costs nothing here.
        mov     eax,dword ptr [park_psize]
        add     eax,15
        shr     eax,4
        add     eax,10h + 100h + 100h
        mov     ebx,eax
        call    lm_alloc
        jc      plp_bad
        mov     word ptr [park_pspseg],ax

        ; The PSP: an INT 20h at the front, the block's end at 2, and
        ; the rest zero.  RMDOS fills the handle table in for itself.
        movzx   edi,ax
        shl     edi,4
        push    edi
        mov     ecx,80h / 4
        xor     eax,eax
        rep     stosd
        pop     edi
        mov     word ptr [edi],20CDh
        movzx   eax,word ptr [park_pspseg]
        dec     eax
        shl     eax,4
        mov     ax,word ptr [eax + RMCB_LEN]
        movzx   ecx,word ptr [park_pspseg]
        add     ax,cx
        inc     ax
        mov     word ptr [edi+2],ax
        mov     byte ptr [edi+80h],0            ; an empty command tail
        mov     byte ptr [edi+81h],13

        ; ...and the image at PSP:0100
        movzx   edx,word ptr [park_pspseg]
        shl     edx,4
        add     edx,100h
        movzx   ebx,word ptr [park_hnd]
        mov     ecx,dword ptr [park_psize]
        mov     eax,3F00h
        call    k21
        jc      plp_bad

        movzx   ebx,word ptr [park_hnd]
        mov     eax,3E00h
        call    k21
        mov     word ptr [park_hnd],0
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
plp_bad:
        movzx   ebx,word ptr [park_hnd]
        mov     eax,3E00h
        call    k21
        mov     word ptr [park_hnd],0
plp_bad2:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; park_xmswho - who is holding the arena.
;
; PARKWIN parks the same DOS on the same machine and hands over 13600K.
; WIN.COM hands over nothing, and not because the arena is in pieces:
; largest and total are both 512 bytes, so it is FULL.  Something
; between COMMAND.COM starting WIN.COM and WIN.COM asking to park takes
; all thirteen megabytes, and freeing the parking program's XMS handles
; does not get any of it back - so it was not taken through the XMS
; host, or not under an owner that matches.
;
; So look.  The largest ALLOCATED block, its owner, and the owner the
; park is running as, which together say whether this is the parking
; program's own memory under a name the reclaim did not recognise, or
; somebody else's entirely.
;---------------------------------------------------------------------
park_xmswho:
        pushad
        xor     ebx,ebx                         ; biggest so far
        xor     edi,edi                         ; and its owner
        mov     esi,ARENA_BASE
pxw_walk:
        cmp     dword ptr [esi+MCB_SIG],SIG_END
        je      pxw_done
        cmp     dword ptr [esi+MCB_SIG],SIG_FREE
        je      pxw_next
        cmp     dword ptr [esi+MCB_SIZE],ebx
        jbe     pxw_next
        mov     ebx,dword ptr [esi+MCB_SIZE]
        mov     edi,dword ptr [esi+MCB_OWNER]
pxw_next:
        mov     eax,dword ptr [esi+MCB_SIZE]
        add     eax,MCB_LEN
        add     esi,eax
        cmp     esi,dword ptr [arena_top]
        jb      pxw_walk
pxw_done:
        mov     dword ptr [park_xmsbig],ebx
        mov     dword ptr [park_xmsbigown],edi
        mov     eax,dword ptr [cur_own]
        mov     dword ptr [park_xmscur],eax
        popad
        ret

;---------------------------------------------------------------------
; park_xmsreclaim - take back the extended memory the parking program
;                   is holding through PM-DOS's OWN XMS host.
;
; This is what stopped Windows 3.1 standard mode on a 16MB machine, and
; it took an XMS log to see because none of it goes through INT 21h.
; The trace reads:
;
;       POOL base 00000000  0 KB
;       arena_avail said 00000200 - 512 bytes
;       AH=08 free?  -> AX=0000  FAILED BL=A0
;       AH=09 alloc  DX=0024 -> FAILED BL=A0
;
; WIN.COM asks RMDOS for 36K, is told there is none, prints
; "Insufficient extended memory to run Windows in standard mode" and
; leaves.  And RMDOS is telling the truth: park_xmsgrab found 512 bytes
; in the arena.  PARKTEST parking the same DOS on the same machine
; hands over 13600K, so the arena is not small - it is EMPTY, and
; WIN.COM emptied it before it parked, by allocating extended memory
; from the host it found at the time, which was PM-DOS's.
;
; Those blocks cannot survive the park in any case.  The park replaces
; the XMS host: PM-DOS's goes out of reach the moment the excursion
; starts and RMDOS's takes over, so a handle issued by one is not a
; handle the other has ever heard of.  Holding the memory for them
; keeps it away from BOTH.  And Windows does not want it kept: the log
; shows it asking the new host how much there is and allocating again
; from scratch, which is exactly what a program does when it has just
; been handed a different DOS.
;
; NOT BY OWNER, which is where the first attempt at this went wrong.
; It followed xms_relown and compared each handle against cur_own, and
; freed nothing at all: the probe above says the arena is held in ONE
; block of 13734K by owner 3, while the park is running as owner 4.
; The memory belongs to the program that STARTED the handover, not to
; the one making it, and in standard mode those are two different
; programs.
;
; So every live handle goes.  That is not a liberty: the excursion
; replaces the XMS host wholesale, and a handle issued by PM-DOS is
; not a handle RMDOS has ever heard of, so between 1605h and 1606h
; there is nobody who can use one.  Real DOS never faces this because
; HIMEM is a TSR and simply stays put across the park; PM-DOS s host
; is part of a kernel that steps out of the room.
;---------------------------------------------------------------------
park_xmsreclaim:
        pushad
        mov     esi,OFFSET xms_handles
        mov     ecx,XMS_MAXH
pxr_one:
        cmp     dword ptr [esi+XH_FLAGS],0
        je      pxr_next
        push    ecx
        push    esi
        mov     edx,dword ptr [esi+XH_ADDR]
        call    arena_free
        pop     esi
        pop     ecx
        mov     dword ptr [esi+XH_FLAGS],0
        mov     dword ptr [esi+XH_LOCK],0
pxr_next:
        add     esi,XH_LEN
        loop    pxr_one
        popad
        ret

;---------------------------------------------------------------------
; park_xmsgrab - take the arena's free memory, to hand over as XMS.
;
; RMDOS cannot work extended memory out for itself the way a real
; HIMEM does, by asking the BIOS how much there is: most of what the
; BIOS would report is the kernel this excursion has to come back to,
; or arena blocks with live data in them.  So PM-DOS decides.  It
; allocates the free memory TO ITSELF - one ordinary arena block, in
; the ordinary way - and hands the block over.  Everything RMDOS gives
; away then comes out of memory the kernel has already agreed to lose,
; and the kernel's own is not reachable through any XMS handle.
;
; It is given back in park_release, which is the same call that gives
; back RMDOS's own image, so an excursion that fails anywhere still
; returns the memory.
;---------------------------------------------------------------------
PARK_XMSKEEP    EQU     128 * 1024      ; left behind for the kernel
PARK_XMSMIN     EQU     64 * 1024       ; below this it is not worth having

park_xmsgrab:
        pushad
        mov     dword ptr [park_xmsblk],0
        mov     dword ptr [park_xmsbase],0
        mov     dword ptr [park_xmslen],0

IFDEF DBG_LPT
        call    park_xmswho                     ; and who is holding the rest
        call    xms_total                       ; ...and every free byte added
        mov     dword ptr [park_xmstotal],eax   ; up, because largest-versus-total
                                                ; is the difference between an
                                                ; arena that is FULL and one that
                                                ; is merely in pieces
ENDIF
        call    arena_avail
        mov     dword ptr [park_xmsavail],eax   ; recorded whatever happens: a
                                                ; pool of zero is the same header
                                                ; whether the arena was empty or
                                                ; arena_alloc lost the race
        cmp     eax,PARK_XMSKEEP + PARK_XMSMIN + 4096
        jb      pxg_none
        sub     eax,PARK_XMSKEEP
        mov     ebx,eax
        call    arena_alloc
        jc      pxg_none                ; someone else got there first
        mov     dword ptr [park_xmsblk],eax

        ; BOTH ENDS ON A PAGE.  XMS counts in kilobytes, so a pool
        ; measured to the kilobyte looks right - and is not, because
        ; the driver hands out page-aligned blocks and would round the
        ; last request up past the end of a pool whose length is an odd
        ; number of kilobytes.  Asking for exactly as much as the
        ; manager says is free is the FIRST thing Windows does.
        lea     edx,[eax+ebx]           ; one past the end
        add     eax,0FFFh
        and     eax,NOT 0FFFh
        cmp     eax,edx
        jae     pxg_none                ; rounded the whole thing away
        mov     dword ptr [park_xmsbase],eax
        sub     edx,eax
        and     edx,NOT 0FFFh
        mov     dword ptr [park_xmslen],edx
        popad
        ret
pxg_none:
        popad
        ret

;---------------------------------------------------------------------
; park_fillhdr - write the handoff block into the loaded image.
;---------------------------------------------------------------------
park_fillhdr:
        pushad
        movzx   edi,word ptr [park_seg]
        shl     edi,4                   ; the image, flat

        mov     dword ptr [edi + RMH_SIG],RMDOS_SIG
        mov     word ptr [edi + RMH_VER],RMDOS_VER

        call    dword ptr ds:[IOS_UNPARKADDR]
        mov     word ptr [edi + RMH_UNPARKOFF],ax
        shr     eax,16
        mov     word ptr [edi + RMH_UNPARKSEG],ax

        mov     word ptr [edi + RMH_MCBHEAD],V86_TPA_SEG
        mov     ax,word ptr [park_pspseg]
        or      ax,ax
        jnz     pfh_psp
        mov     ax,word ptr [park_psp]  ; a guest we are resuming: its own
        or      ax,ax
        jnz     pfh_psp
        mov     ax,V86_TPA_SEG          ; nothing of ours is running: the
                                        ; arena's own first block will do
pfh_psp:
        mov     word ptr [edi + RMH_PSP],ax

        mov     eax,[lm_top]                    ; RMH_MEMTOP: declared by both
        shr     eax,4                           ; sides since v0.45 and written
        mov     word ptr [edi + RMH_MEMTOP],ax  ; by neither until now

        movzx   eax,byte ptr ds:[GV_BOOTDRV]
        cmp     al,80h
        jb      pfh_fd
        sub     al,80h - 2
pfh_fd:
        mov     byte ptr [edi + RMH_BOOTDRV],al
        mov     al,[cur_drive]
        mov     byte ptr [edi + RMH_CURDRV],al
        mov     al,[drv_count]
        cmp     al,LASTDRIVE
        jbe     pfh_ndrv
        mov     al,LASTDRIVE
pfh_ndrv:
        mov     byte ptr [edi + RMH_NDRV],al

        ; VERBOSE ONLY WHEN PM-DOS STARTED THE PROGRAM.  Announcing
        ; itself is what makes RMDOS testable - PARKWIN.COM's screen is
        ; the whole of that test - but on the frame path the program
        ; being resumed is Windows, which has already taken the screen
        ; and put a 640x480 logo on it.  A banner there is not a
        ; message, it is a band of corruption across somebody's
        ; graphics, printed by a DOS the user did not know had started.
        mov     al,RMHF_VERBOSE
        cmp     dword ptr [park_prog],0
        jne     pfh_verb
        xor     al,al
pfh_verb:
        ; AND WHERE IT IS, WHICH IT CANNOT WORK OUT SAFELY FOR ITSELF.
        ; RMDOS could read its own CS and compare it to FFFFh, but the
        ; thing this flag guards is the A20 gate, and a driver that
        ; got that test wrong would find out by executing the
        ; interrupt vector table.  Being told is better.
        cmp     byte ptr [park_high],0
        je      pfh_flagout
        or      al,RMHF_HIGH
pfh_flagout:
        mov     byte ptr [edi + RMH_FLAGS],al

        mov     ax,word ptr [park_pspseg]
        or      ax,ax
        jz      pfh_noprog
        mov     word ptr [edi + RMH_RUNSEG],ax
        mov     word ptr [edi + RMH_RUNOFF],100h
        mov     word ptr [edi + RMH_RUNSS],ax
        mov     word ptr [edi + RMH_RUNSP],0FFFEh
        jmp     short pfh_drives

;--- No program to start, so RMDOS takes the other branch and IRETs to
;--- the frame instead.  It reads the frame OUT OF ITS OWN IMAGE, so
;--- the staging copy has to be moved in here with everything else -
;--- after the mode switch there is no protected-mode code left to do
;--- it, and memory is the only thing that survives the switch.
;
;--- A LOOP, WHERE A REP MOVSB WOULD BE ONE INSTRUCTION.  A string move
;--- writes through ES, and every other write in this routine goes
;--- through DS - which is why the REP MOVSB that used to be here was
;--- the only line that did not arrive.  It failed silently and it
;--- failed convincingly: RMDOS started, checked a signature this
;--- routine HAD written, found it, ran its whole initialisation and
;--- IRETed onto a frame of zeroes, which sent the guest to 0000:0000
;--- to execute the interrupt vector table.  Nothing about that looks
;--- like a segment register.
pfh_noprog:
        mov     word ptr [edi + RMH_RUNSEG],0
        push    ecx
        push    esi
        mov     esi,OFFSET park_iret
        xor     ecx,ecx
pfh_frame:
        mov     eax,dword ptr [esi + ecx*4]
        mov     dword ptr [edi + RMH_IRET + ecx*4],eax
        inc     ecx
        cmp     ecx,RMI_LEN / 4
        jb      pfh_frame
        pop     esi
        pop     ecx
pfh_drives:
        mov     word ptr [edi + RMH_EXITCODE],0
        mov     word ptr [edi + RMH_WHY],RMW_NOTOURS

        mov     eax,dword ptr [park_xmsbase]
        mov     dword ptr [edi + RMH_XMSBASE],eax
        mov     eax,dword ptr [park_xmslen]
        mov     dword ptr [edi + RMH_XMSLEN],eax
IFDEF DBG_LPT
        ; The two spare header dwords, carrying the ownership probe.
        ; Largest and total are both known to be 512 bytes in the case
        ; being chased, so the space is better spent on WHOSE.
        mov     eax,dword ptr [park_xmsbigown]
        mov     dword ptr [edi + RMH_RSVD],eax
        mov     eax,dword ptr [park_xmsbig]
        shr     eax,10                          ; KB - it fits in sixteen bits
        mov     edx,dword ptr [park_xmscur]
        shl     edx,16
        or      eax,edx
        mov     dword ptr [edi + RMH_RSVD + 4],eax
ENDIF

        ; The drive table.  PM-DOS scanned the partitions; this is the
        ; answer, in the two fields RMDOS needs to find each volume's
        ; boot sector and read the rest for itself.
        movzx   ecx,byte ptr [edi + RMH_NDRV]
        or      ecx,ecx
        jz      pfh_done
        mov     esi,OFFSET drv_tbl
        lea     ebx,[edi + RMH_DRIVES]
pfh_one:
        mov     al,[esi+DRV_BIOS]
        mov     [ebx+RMD_BIOS],al
        xor     al,al
        test    byte ptr [esi+DRV_FLAGS],DRVF_PRESENT
        jz      pfh_flags
        or      al,RMDF_PRESENT
        test    byte ptr [esi+DRV_FLAGS],DRVF_FLOPPY
        jz      pfh_flags
        or      al,RMDF_FLOPPY
pfh_flags:
        ; A DRIVER-served unit has no BIOS drive number and no boot
        ; sector RMDOS could find, so it is simply not there on the
        ; other side.  Saying so is better than handing over a number
        ; that names a real disk by accident.
        test    byte ptr [esi+DRV_FLAGS],DRVF_DRIVER
        jz      pfh_notdrv
        xor     al,al
pfh_notdrv:
        mov     [ebx+RMD_FLAGS],al
        mov     eax,dword ptr [esi+DRV_START]
        mov     dword ptr [ebx+RMD_START],eax
        add     esi,DRV_LEN
        add     ebx,RMD_LEN
        loop    pfh_one
pfh_done:
        popad
        ret

;---------------------------------------------------------------------
; park_go - flush everything, then leave.
;---------------------------------------------------------------------
park_go:
        pushad

        ; EVERYTHING DIRTY GOES OUT FIRST.  RMDOS is about to write to
        ; the same disks through the same BIOS, and a cached cluster up
        ; here that has not reached the platter is a write that RMDOS
        ; cannot see and that will land on top of its work afterwards.
        call    fat_flush

        movzx   eax,word ptr [park_seg]
        mov     word ptr [park_blk + GVP_CS],ax
        mov     word ptr [park_blk + GVP_IP],RM_ORG     ; not 0: the image
                                        ; starts sixteen bytes into its own
                                        ; segment, and RMH_JMP is its first
                                        ; instruction
        mov     word ptr [park_blk + GVP_DS],ax
        mov     word ptr [park_blk + GVP_ES],ax
        mov     word ptr [park_blk + GVP_SS],ax
        movzx   eax,word ptr [park_paras]
        shl     eax,4
        sub     eax,2
        cmp     eax,0FFFEh
        jbe     pg_sp
        mov     eax,0FFFEh
pg_sp:
        mov     word ptr [park_blk + GVP_SP],ax
        mov     byte ptr [park_blk + GVP_ACTIVE],0
        mov     word ptr [park_blk + GVP_AX],0

IFDEF RMDOS_BISECT
        ; WHAT IS ACTUALLY AT THE TWO ADDRESSES THAT MATTER.  Reading
        ; linear 100000h proves nothing on its own - if the A20 alias
        ; is still pointing at zero, that read follows the image to
        ; wherever it really went and reports success either way.  So
        ; read linear 0 as well.  The image begins E9 xx xx 'R' 'M'
        ; 'D': see it at 100000h and it is where it was meant to go,
        ; see it at 0 and it landed on the interrupt vector table.
        movzx   eax,word ptr [park_seg]
        mov     edi,0B8000h + 60
        call    park_mhex
        mov     eax,dword ptr ds:[00100000h]    ; should be the E9 of the jmp
        mov     edi,0B8000h + 100
        call    park_mhex
        mov     eax,dword ptr ds:[00100010h]    ; and if the E9 is HERE, the
        mov     edi,0B8000h + 120               ; whole image is 16 too high
        call    park_mhex
        mov     word ptr ds:[0B8000h + 140],1F31h       ; '1' - about to go
ENDIF
        mov     esi,OFFSET park_blk
        call    dword ptr ds:[IOS_PARK]
IFDEF RMDOS_BISECT
        mov     word ptr ds:[0B8000h + 150],1F35h       ; '5' - and back
ENDIF

        ; ...and we are back in protected mode, with paging on, at the
        ; instruction after the call.
        movzx   edi,word ptr [park_seg]
        shl     edi,4
        mov     ax,word ptr [edi + RMH_EXITCODE]
        mov     word ptr [park_exit],ax
        mov     ax,word ptr [edi + RMH_WHY]
        mov     word ptr [park_why],ax
        popad
        ret

IFDEF RMDOS_BISECT
;--- EAX in hex at EDI, straight into the text page.  Eight digits,
;--- high nibble first, and no service call anywhere in it: this runs
;--- with the machine half-way out of protected mode's usefulness and
;--- the console is one of the things the excursion is about to take.
park_mhex:
        pushad
        mov     ecx,8
pmh_one:
        rol     eax,4
        mov     bl,al
        and     bl,0Fh
        add     bl,'0'
        cmp     bl,'9'
        jbe     pmh_dig
        add     bl,'A' - '0' - 10
pmh_dig:
        mov     byte ptr ds:[edi],bl
        mov     byte ptr ds:[edi+1],1Fh
        add     edi,2
        loop    pmh_one
        popad
        ret
ENDIF

;=====================================================================
; park_chaincut - end the MCB chain at the top of conventional memory,
;                 and park_chainjoin puts it back.
;
; UPPER MEMORY EXISTS BECAUSE OF PAGING, AND THE PARK TURNS PAGING OFF.
; PM-DOS's UMBs are ordinary RAM mapped into the holes between A0000h
; and 100000h, and the MCB chain covers the whole span - a chain is
; contiguous, so it has to, which is what v0.38 found out.  The moment
; the kernel drops paging, every one of those addresses goes back to
; meaning what the hardware says it means: video memory and ROM.  The
; blocks are still described by a chain that says they are memory.
;
; So a walk from the head runs off the end of conventional memory into
; the frame buffer, reads whatever the VGA answers with as a block
; header, and stops - or does not stop.  RMTEST could not walk it.
; Windows walks it too, to find out how much conventional memory there
; is, and the answer it got was small enough that it said so and quit.
;
; For the length of the excursion the machine genuinely HAS no upper
; memory, so this is not a trick to get past a check: it is the chain
; telling the truth for once.  The last block below the ceiling becomes
; the last block, and gets its 'M' back on the way home.
;=====================================================================
park_chaincut:
        pushad
        mov     word ptr [park_cutseg],0
        mov     eax,[lm_top]
        shr     eax,4                   ; first paragraph above conventional
        mov     ebx,V86_TPA_SEG
        xor     edx,edx                 ; the last block seen below it
pcc_walk:
        cmp     ebx,eax
        jae     pcc_found
        mov     esi,ebx
        shl     esi,4
        cmp     byte ptr [esi+RMCB_SIG],'M'
        jne     pcc_done                ; a 'Z' below the ceiling already
                                        ; ends it, and anything else is not
                                        ; a chain we should be editing
        mov     edx,ebx
        movzx   ecx,word ptr [esi+RMCB_LEN]
        add     ebx,ecx
        inc     ebx
        jmp     short pcc_walk
pcc_found:
        or      edx,edx
        jz      pcc_done
        mov     esi,edx
        shl     esi,4
        mov     word ptr [park_cutseg],dx
        mov     byte ptr [esi+RMCB_SIG],'Z'
pcc_done:
        popad
        ret

;---------------------------------------------------------------------
; park_chainjoin - and give upper memory back.
;
; NOT BY REMEMBERING WHICH BLOCK WAS CUT.  RMDOS allocates out of this
; arena the whole time it is parked, and a split gives the TAIL block
; the marker the original carried - so the block that ends the chain
; when we come back is very often not the block that was ended, and
; putting the 'M' back where it was leaves the real 'Z' in the middle
; with upper memory orphaned behind it.  That is exactly what the
; first version of this did, and UMBTEST said so in three checks.
;
; So find the end of the chain as it is NOW, and only mend it if it
; ends exactly where conventional memory does - which is the one
; place this routine put a 'Z'.  A chain ending anywhere else is
; something else's problem and is left alone.
;---------------------------------------------------------------------
park_chainjoin:
        pushad
        cmp     word ptr [park_cutseg],0
        je      pcj_done
        mov     word ptr [park_cutseg],0
        mov     eax,[lm_top]
        shr     eax,4                   ; the paragraph the chain must reach
        mov     ebx,V86_TPA_SEG
pcj_walk:
        mov     esi,ebx
        shl     esi,4
        movzx   ecx,word ptr [esi+RMCB_LEN]
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        je      pcj_last
        cmp     byte ptr [esi+RMCB_SIG],'M'
        jne     pcj_done                ; not a chain we should be editing
        add     ebx,ecx
        inc     ebx
        cmp     ebx,eax
        jb      pcj_walk
        jmp     short pcj_done          ; already runs past the ceiling
pcj_last:
        add     ebx,ecx
        inc     ebx
        cmp     ebx,eax
        jne     pcj_done                ; ends somewhere we did not end it
        mov     byte ptr [esi+RMCB_SIG],'M'
pcj_done:
        popad
        ret

;---------------------------------------------------------------------
; park_recover - put back what the excursion changed.
;---------------------------------------------------------------------
park_recover:
        pushad
        call    park_chainjoin          ; upper memory exists again

        ; The DOS vectors a guest reflects through.  RMDOS pointed them
        ; at itself, and itself is about to be freed.
        call    vdm_stubs

        ; Every volume, again.  RMDOS has been writing to these disks
        ; and every FAT window and cluster cached up here describes the
        ; disk as it was before it started.
        call    park_remount

        popad
        ret

; The same sequence 21h/F1h AL=2 uses on one drive, on all of them:
; swap the volume's state in, throw away every cached thing about it,
; and mount it again.  A failed mount is not an error here - a floppy
; that is not in the drive was not readable before we parked either.
;
; AND "NOT AN ERROR" HAS TO BE SAID TO THE CRITICAL-ERROR MACHINERY,
; not just meant.  This walks EVERY drive letter, empty ones included,
; and a mount reads the medium; on an empty floppy that read fails and
; raises a critical error, which prompts - "Not ready reading drive A,
; Abort, Retry, Fail?" - on behalf of a program that asked for none of
; this, at a moment when the screen may belong to Windows and there
; may be nobody to read it.  crit_busy is the flag that makes
; crit_raise answer FAIL and return, and this is exactly the case it
; is for: housekeeping with no caller to ask.
;
; It went unnoticed for four releases because the unpark path had
; never run on a machine with an empty drive in it - the same reason,
; and very nearly the same bug, as the 4408 critical error v0.48 found
; on the way IN to Windows.  Both ends of the excursion now know that
; a drive which is not there is an answer and not a question.
park_remount:
        push    eax
        push    ecx
        movzx   eax,byte ptr [crit_busy]
        push    eax
        mov     byte ptr [crit_busy],1
        xor     ecx,ecx
pr_loop:
        cmp     cl,MAX_DRIVES
        jae     pr_done
        mov     al,cl
        call    vol_select
        mov     byte ptr [fatsec_dirty],0
        mov     byte ptr [fat12_dirty],0
        mov     dword ptr [fatsec_lba],-1
        mov     dword ptr [sec_lba],-1
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [dc_curord],-1
        mov     byte ptr [fat_ok],0
        mov     al,cl
        call    vol_select              ; and mount it afresh
        inc     cl
        jmp     short pr_loop
pr_done:
        pop     eax
        mov     byte ptr [crit_busy],al ; back to whatever it was, which is
                                        ; zero unless a handler is running
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; park_release - give the low memory back
;---------------------------------------------------------------------
park_release:
        pushad
        ; The guest's A20 back the way the guest left it - see park_run.
        ; Unconditional, and saved at the top of park_run rather than
        ; where it is changed, so that the paths which fail before the
        ; change restore the value they saved rather than last time's.
        mov     al,byte ptr [park_a20]
        call    dword ptr ds:[IOS_A20]

        mov     edx,dword ptr [park_xmsblk]
        or      edx,edx
        jz      prl_noxms
        call    arena_free
        mov     dword ptr [park_xmsblk],0
        mov     dword ptr [park_xmsbase],0
        mov     dword ptr [park_xmslen],0
prl_noxms:
        mov     ax,word ptr [park_pspseg]
        or      ax,ax
        jz      prl_nop
        call    lm_free
        mov     word ptr [park_pspseg],0
prl_nop:
        ; THE HMA WAS NEVER ALLOCATED, SO IT IS NOT FREED.  It is not
        ; in the arena and lm_free has no block to find; handing it a
        ; segment of FFFFh would be asking it to walk an MCB chain
        ; from the ROM.
        cmp     byte ptr [park_high],0
        jne     prl_wasHMA
        mov     ax,word ptr [park_seg]
        or      ax,ax
        jz      prl_none
        call    lm_free
prl_wasHMA:
        mov     byte ptr [park_high],0
        mov     word ptr [park_seg],0
prl_none:
        popad
        ret

;=====================================================================
; Data
;=====================================================================
park_name   db  '\RMDOS.SYS',0
park_prog   dd  0
park_seg    dw  0
park_pspseg dw  0
park_hnd    dw  0
park_size   dd  0
park_paras  dw  0
park_psize  dd  0
park_exit   dw  0
park_why    dw  0
park_blk    db  GVP_LEN dup(0)
park_xmsblk  dd  0                      ; the arena block, for giving back
park_xmsbig    dd 0                   ; the largest allocated block,
park_xmsbigown dd 0                   ; whose it is,
park_xmscur    dd 0                   ; and who the park is running as
park_xmstotal dd 0                     ; every free byte in the arena
park_xmsavail dd 0                     ; and what arena_avail said, which
                                                ; is the only way to tell an empty
                                                ; arena from a failed allocate
park_xmsbase dd  0                      ; and the aligned range inside it
park_xmslen  dd  0
park_lderr   dd  ERR_NOFILE             ; WHERE in park_loadrm it went
                                        ; wrong - every one of its six
                                        ; failures used to say "no file"
park_high    db  0                      ; the image went in the HMA, so it
                                        ; is not the arena's to give back
park_cutseg  dw  0                     ; the block whose 'M' became a 'Z'
park_a20     db  0                      ; the guest's emulated gate, which
                                        ; the excursion has to force open
                                        ; and then hand back
park_psp     dw  0                      ; the guest process to resume, and
park_iret    db  RMI_LEN dup(0)         ; the registers to resume it with
