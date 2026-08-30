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

        call    park_xmsgrab            ; the extended memory to hand over
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
        or      eax,eax
        jz      plr_bad
        cmp     eax,64 * 1024
        jae     plr_bad                 ; it has to fit in one segment, and
                                        ; everything in it assumes so

        movzx   ebx,word ptr [park_hnd]
        mov     eax,4200h
        xor     ecx,ecx
        xor     edx,edx
        call    k21
        jc      plr_bad

        ; Room for it, plus a margin
        mov     eax,dword ptr [park_size]
        add     eax,15
        shr     eax,4
        add     eax,PARK_SLACK
        mov     ebx,eax
        mov     dword ptr [park_lderr],ERR_NOMEM
        mov     word ptr [park_paras],ax        ; BEFORE the call: lm_alloc
                                        ; does not preserve EBX, and on
                                        ; failure it deliberately puts
                                        ; the largest free run there
        call    lm_alloc                ; -> AX = segment
        jc      plr_bad
        mov     word ptr [park_seg],ax

        ; Read it straight into conventional memory.  The address is
        ; flat here because the kernel is flat; it is the same bytes
        ; the real-mode side will address as a segment.
        movzx   edx,word ptr [park_seg]
        shl     edx,4
        movzx   ebx,word ptr [park_hnd]
        mov     ecx,dword ptr [park_size]
        mov     eax,3F00h
        call    k21
        jc      plr_bad
        cmp     eax,dword ptr [park_size]
        jne     plr_bad

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

        call    arena_avail             ; -> EAX, the largest single block
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
        mov     word ptr [park_blk + GVP_IP],0
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

        mov     esi,OFFSET park_blk
        call    dword ptr ds:[IOS_PARK]

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

;---------------------------------------------------------------------
; park_recover - put back what the excursion changed.
;---------------------------------------------------------------------
park_recover:
        pushad

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
        mov     ax,word ptr [park_seg]
        or      ax,ax
        jz      prl_none
        call    lm_free
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
park_xmsbase dd  0                      ; and the aligned range inside it
park_xmslen  dd  0
park_lderr   dd  ERR_NOFILE             ; WHERE in park_loadrm it went
                                        ; wrong - every one of its six
                                        ; failures used to say "no file"
park_psp     dw  0                      ; the guest process to resume, and
park_iret    db  RMI_LEN dup(0AAh)      ; the registers to resume it with
