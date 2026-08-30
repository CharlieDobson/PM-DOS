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
;                   under RMDOS, or 0 to resume the frame at
;                   [park_frame] instead (which is the Windows case).
;
; Returns EAX = the exit code, CF set if the excursion never happened.
;=====================================================================
park_run:
        push    ebx
        push    ecx
        push    edx
        push    edi
        push    ebp
        mov     dword ptr [park_prog],esi

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
        mov     eax,ERR_NOFILE
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
        mov     edx,OFFSET park_name
        mov     eax,3D00h
        call    k21
        jc      plr_nofile
        mov     word ptr [park_hnd],ax

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
        mov     byte ptr [edi + RMH_FLAGS],RMHF_VERBOSE

        mov     ax,word ptr [park_pspseg]
        or      ax,ax
        jz      pfh_noprog
        mov     word ptr [edi + RMH_RUNSEG],ax
        mov     word ptr [edi + RMH_RUNOFF],100h
        mov     word ptr [edi + RMH_RUNSS],ax
        mov     word ptr [edi + RMH_RUNSP],0FFFEh
        jmp     short pfh_drives
pfh_noprog:
        mov     word ptr [edi + RMH_RUNSEG],0
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
park_remount:
        push    eax
        push    ecx
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
