;=====================================================================
; RMSTK.INC - RMDOS's stack slots, where linear 10000h falls, and the
;             start-up code that is assembled in the bottom of them.
;
; Included from RMDOS.ASM, between RMUSB.INC and RMXMS2.INC.  Read
; PINNED AT RD_STKORG and NEXT TO THE SLOTS below before anything
; around it is moved.
;=====================================================================

;--- THE STACKS, AND rd_lowkeep IN THE BOTTOM OF THEM.  rd_lowkeep is
;--- the kernel's current directories for our volumes and NUL's link,
;--- copied out of the block below 64K by rd_lowsave before rd_sysinit
;--- writes over it - and rd_sysinit and rd_chain are the only readers
;--- it has, both before the handover is finished.  No call can be
;--- standing on a slot by then, so its 2K are the slots' 2K: it was a
;--- buffer of its own until 2026-10-23, and the 2K it took are the
;--- segment's room for code now (SETVER took the first of it).  The
;--- start-up runs on rd_stktop, the top of the LAST slot, and the
;--- slots under rd_lowkeep are marked taken in rd_stkmap until
;--- rd_sysinit is done, so that not even a call made from an
;--- interrupt can be given one (rd_entry, RD_KEEPMAP).
;---
;--- WHERE THEY ARE IS WHERE LINEAR 10000h IS (2026-11-20).
;--- Windows 3.1 with a WINSTART.BAT leaves on a stack of 0000:0000.  Its
;--- DOSMGR EXECs twice in the System VM - COMMAND /C WINSTART, then
;--- KRNL386 - and when an EXEC returns it puts the client's SS:SP back
;--- from the word it gave Call_When_VM_Returns.  For the first EXEC that
;--- word is SS:SP.  For the second it is whatever _HeapFree left in EDX,
;--- which is nought (WIN386.EXE 3.10, DOSMGR at file offset 5F512h; WfW
;--- 3.11's has a PUSH EDX and a POP EDX there).  So SHELL's 4Dh, the
;--- exit broadcasts, the mouse reset, 0C06h and every XMS free are made
;--- with SP = 0, and each of them writes down from linear 10000h.  It
;--- does the same on MS-DOS, where that is memory KRNL386 has just given
;--- back.  Here it is this segment, which starts below 64K and is 64K
;--- long; while the slots were at the end of the image the bytes under
;--- the line were whatever code the build had put there, and Windows
;--- 3.1 hung on the way out as soon as that was code the exit needed.
;---
;--- So the slots are where the line falls for the segment RMDOS is
;--- given below Windows - 9350h from 06CBh, the kernel alone below it,
;--- and lower by what CONFIG.SYS adds - and the slot the line is in is
;--- the stray stack's: rd_entry works out which (rd_linemap), with the
;--- slot under it too when the line is less than RD_LINEMIN bytes up
;--- from that slot's foot, and the first call that arrives with SS =
;--- 0000 takes them out of use for good (i21).  Until then each is a
;--- slot like the others.  A call standing on one at that moment would
;--- lose its frame; there is none - Windows does not leave while another
;--- machine is running, and the System VM's own last call, KRNL386's
;--- 4Ch, has given its slot back.  A segment that puts the line above
;--- the slots or below them is as it was before: the line is in code.
;---
;--- PINNED AT RD_STKORG (2026-11-21).  Where the line falls depends on
;--- the segment and on nothing else, so the slots have to stay at the
;--- offset they were tested at, whatever is added to the code in front
;--- of them.  The ORG below puts rd_stks at RD_STKORG, and the build
;--- stops when the code in front has grown past it.  What to do then is
;--- to take code from in front and put it behind.  RMXMS.INC is cut in
;--- two for that and for no other reason - its first part in front,
;--- RMXMS2.INC behind - and the cut can be moved to another routine.
;--- The bytes the ORG skips are the room for code in front; the room
;--- the check at rd_imgend counts is the room for code behind.  A trace
;--- build has more in front than any place for the line allows, and
;--- its slots float (RD_STKFLOAT, RMDOS.ASM): they are where its code
;--- ends, and rd_entry still works out which slot is under the line,
;--- if one is.
;---
;--- START-UP CODE IN THE SLOTS (2026-11-21).  rd_sysinit and
;--- rd_cdletter run once, from rd_entry, while rd_stkmap still says
;--- RD_KEEPMAP: the slots rd_lowkeep is in are taken, and the room
;--- above rd_lowkeep in the last of them was nobody's.  So the two are
;--- assembled there - as rd_lowkeep's own 2K were given to the slots on
;--- 2026-10-23 - and the bytes they had in RMINFO.INC are the segment's
;--- room again.  The image is read from the disk at every park
;--- (park_loadrm), so the code is there each time Windows starts.
;--- NOTHING HERE MAY BE CALLED ONCE rd_entry HAS CLEARED rd_stkmap: the
;--- first call that is given the slot writes its frame over this code.
;--- RD_INITLEN is the room the code is counted to have, RD_KEEPSLOTS is
;--- worked out from rd_lowkeep and that together, and the check after
;--- the code stops a build in which the code has outgrown RD_INITLEN.
;---
;--- NEXT TO THE SLOTS, AND WHY rd_entry CLEARS THE START-UP CODE
;--- (2026-11-21).  An emulator that translates code - QEMU's is the one
;--- measured - watches every 4K page it has translated code from, and
;--- while any of that code stands as it was run, EVERY WRITE TO THE
;--- PAGE takes the slow way.  A PUSH and a POP on a stack in such a page
;--- took a hundred times as long (scratch t\SMCT.COM: 64,000,000 pairs
;--- in 6 ticks on a clean page; 640,000 in 6 once a RET in the page had
;--- been called; 64,000,000 in 6 again when the RET was written over).
;--- A real processor does none of this.  Windows' calls run on the low
;--- slots - slot 0 holds DOSMGR's EXEC of KRNL386 for as long as Windows
;--- runs, the System VM's calls are on slot 1, a DOS box's come after -
;--- and with rd_sysinit left standing in slot 2 all of them were in a
;--- page of "code": Windows took minutes over its first file, and the
;--- test runs timed out.  So:
;---   - rd_entry writes noughts over the start-up code as soon as it
;---     has returned, which is what the slot held before 2026-11-21;
;---   - the code in front of rd_stks is the code that was in front
;---     before - RMNET2.INC, RMNET.INC and RMUSB.INC, which run only
;---     for a network drive or a USB disk.  The first part of RMXMS.INC
;---     had to go in front to keep rd_stks where it is, and is in the
;---     room rd_sysinit left after RMINFO.INC, not next to the slots;
;---   - which bytes of the code in front share a page with a slot
;---     depends on the segment, sixteen bytes to the paragraph.  See
;---     NOTES.TXT, 2026-11-21, before code that Windows runs often is
;---     put next to the slots, on either side.
rd_stkpre:
IF (rd_stkpre - h_sig) + RMH_BASE GT RD_STKORG
IFNDEF RD_STKFLOAT
        .ERR    <RMSTK.INC: the code in front of the stacks is past RD_STKORG - move code behind them>
ENDIF
ELSE
        ORG     RD_STKORG
ENDIF
rd_stks:                                ; slot 0 ends at rd_stks + RD_STKSIZE
rd_lowkeep  db  RD_KEEPLEN dup(0)

;=====================================================================
; rd_sysinit - build the published structures, IN THE BLOCK BELOW 64K.
;
; Everything here goes through ES = [rd_lowseg], the block PM-DOS
; handed over (RMDOS.INC, RMH_LOWSEG), and nothing in it is addressed
; through this file's own segment any more.  The reason is DOSMGR,
; which reads the list of lists' address out of 52h, turns it into a
; linear address and refuses to load if that is 64K or more: a real
; DOS keeps its data at the bottom of memory and Windows was written
; against real DOSes.  rd_lowsave has already kept what the block held.
;=====================================================================
rd_sysinit:
        pusha
        push    es
        mov     es,[rd_lowseg]

        ; HOW MANY LETTERS: LASTDRIVE= as it was written, never fewer
        ; than the volumes there are (DOS6SRC CDSCOUNT, NUMIO), up to Z:
        ; - and all twenty-six when there is no line, which is what the
        ; kernel does with no line too (all of its sixteen), where
        ; MS-DOS would stop at E:.  A letter past the volumes is a
        ; structure nobody uses until a redirector takes it, which is
        ; how Windows for Workgroups maps a network drive; with the
        ; count fixed at eight a logon script's NET USE had no I: to Z:.
        mov     al,[h_lastdrv]
        or      al,al
        jnz     rsi_ldline
        mov     al,RL_NCDS              ; no line: A: to Z:
rsi_ldline:
        mov     cx,LASTDRIVE            ; the highest volume, counted from 1
        mov     si,OFFSET h_drives + (LASTDRIVE - 1) * RMD_LEN
rsi_ldvol:
        test    byte ptr [si+RMD_FLAGS],RMDF_PRESENT
        jnz     rsi_ldhave
        sub     si,RMD_LEN
        loop    rsi_ldvol
rsi_ldhave:
        cmp     al,cl
        jae     rsi_ldcd
        mov     al,cl
rsi_ldcd:
        call    rd_cdletter             ; and the CD-ROM's, if RMCD is here
        jc      rsi_ldfloor
        inc     cl
        cmp     al,cl
        jae     rsi_ldfloor
        mov     al,cl
rsi_ldfloor:
        cmp     al,RL_NCDS
        jbe     rsi_ldcap
        mov     al,RL_NCDS
rsi_ldcap:
        mov     [rd_ncds],al

        ; CLEARED WHERE THIS DOS BUILDS, AND NOT WHERE THE KERNEL'S
        ; ANSWERS ARE STILL TRUE.  The segment is laid out the same on
        ; both sides of the handover (RMDOS.INC), so the tail of the
        ; list of lists - the boot drive, the extended memory - and
        ; UMB_HEAD describe this machine as well as they did a moment
        ; ago, and the country tables are tables.  Everything else up
        ; to the end of the CDS array is rebuilt below.
        cld
        xor     al,al
        xor     di,di
        mov     cx,RL_LOLTAIL
        rep     stosb
        mov     di,RL_LOLEND
        mov     cx,RL_UMBWORD - RL_LOLEND
        rep     stosb
        mov     di,RL_UMBWORD + 2
        mov     cx,RL_NLS - (RL_UMBWORD + 2)
        rep     stosb
        mov     di,RL_NLSEND
        mov     cx,RL_LEN - RL_NLSEND
        rep     stosb
        mov     byte ptr es:[RL_SDA + SD_WPERR],0FFh    ; as MS-DOS does:
                                                        ; drive zero is real
        ; DOS 5's DataVersion, and SHARE's jump table full of RETFs -
        ; see the layout in RMDOS.INC for who reads them and why
        mov     word ptr es:[RL_DATAVER],1
        mov     di,RL_JSHARE
        mov     cx,RL_JSHARECNT
rsi_jshare:
        mov     word ptr es:[di],RL_DEVRET
        mov     word ptr es:[di+2],es
        add     di,4
        loop    rsi_jshare

        mov     ax,[h_mcbhead]
        mov     word ptr es:[RL_LOL + LL_MCBSEG],ax
        mov     dword ptr es:[RL_LOL + LL_DPB],0FFFFFFFFh
        mov     ax,[rd_sfthp]
        mov     word ptr es:[RL_LOL + LL_SFT],ax
        mov     word ptr es:[RL_LOL + LL_SFT + 2],cs    ; a chain, and a
        ; walker follows a chain without checking its head - see
        ; rd_sfthp in RMFILE.INC for what FFFF:FFFF here cost.  And
        ; two blocks long, the second with one entry, because VSHARE
        ; looks one block further than the last - rd_layout built both
        mov     dword ptr es:[RL_LOL + LL_CLOCK],0FFFFFFFFh
        mov     dword ptr es:[RL_LOL + LL_BUFFERS],0FFFFFFFFh
        mov     word ptr es:[RL_LOL + LL_CDS],RL_CDS     ; real now: DOSMGR
        mov     word ptr es:[RL_LOL + LL_CDS + 2],es    ; instances it
        mov     word ptr es:[RL_LOL + LL_FCB],OFFSET rd_fcbtab  ; DOS 5's
        mov     word ptr es:[RL_LOL + LL_FCB + 2],cs    ; FCBS=4,0 (RMFILE.INC)
        mov     word ptr es:[RL_LOL + LL_MAXSEC],SECSIZE
        mov     al,[h_ndrv]
        mov     es:[RL_LOL + LL_NBLOCK],al
        mov     al,[rd_ncds]
        mov     byte ptr es:[RL_LOL + LL_LASTDRV],al

        ; The CON device the list of lists points at is the CON header,
        ; as DOS's does.  It was the NUL header until the kernel's list
        ; had a console pointer worth matching (2026-10-06).  The
        ; headers' two entry points are a RETF in the block itself,
        ; because a device header's entries are offsets in the header's
        ; own segment and that segment is no longer this one.
        mov     word ptr es:[RL_LOL + LL_CON + 2],es
        mov     word ptr es:[RL_LOL + LL_CON],RL_DEVS

        mov     di,RL_LOL + LL_NUL
        mov     dword ptr es:[di],0FFFFFFFFh    ; no next device
        mov     word ptr es:[di+4],8004h        ; character, NUL
        mov     word ptr es:[di+6],RL_DEVRET
        mov     word ptr es:[di+8],RL_DEVRET
        ; THE NAME A BYTE AT A TIME.  This was one dword written as 'LUN ',
        ; on the belief that the assembler would reverse it into "NUL ";
        ; it put " NUL" there instead, and the device header DOSMGR walks
        ; said so for three releases.
        mov     byte ptr es:[di+0Ah],'N'
        mov     byte ptr es:[di+0Bh],'U'
        mov     byte ptr es:[di+0Ch],'L'
        mov     dword ptr es:[di+0Dh],'    '
        mov     byte ptr es:[di+11h],' '
        mov     byte ptr es:[RL_DEVRET],0CBh    ; RETF

        ; AND CON, AUX AND PRN BEHIND IT, headers only: the attribute
        ; word, the name, and the same RETF for both entries.  A DOS
        ; has a device chain and things walk it - DOSMGR, MEM, MSD -
        ; and until v0.63 this one was NUL alone, with every open
        ; handle's SFT_DEVPTR pointing nowhere.  The chain runs NUL,
        ; CON, AUX, PRN, as DOS orders the first four - with the
        ; drivers CONFIG.SYS loaded between NUL and CON (rd_chain).
        call    rd_chain                        ; NUL's next
        call    drv_coninit                     ; and a loaded CON, which
        cmp     word ptr [rd_condrv+2],0        ; is then the list's console
        je      rsi_owncon
        mov     eax,dword ptr [rd_condrv]
        mov     dword ptr es:[RL_LOL + LL_CON],eax
rsi_owncon:
        mov     di,RL_DEVS
        mov     si,OFFSET rsi_devtab
        mov     cx,3
rsi_dev:
        mov     word ptr es:[di],RL_DEVLEN      ; next: the one after
        add     word ptr es:[di],di
        mov     word ptr es:[di+2],es
        cmp     cx,1
        jne     rsi_devnext
        mov     dword ptr es:[di],0FFFFFFFFh    ; ...or the end of the chain
rsi_devnext:
        mov     ax,[si]
        mov     es:[di+4],ax                    ; the attribute word
        mov     word ptr es:[di+6],RL_DEVRET
        mov     word ptr es:[di+8],RL_DEVRET
        push    cx
        push    di
        add     di,0Ah
        add     si,2
        mov     cx,8
        rep     movsb                           ; the name, DS:SI -> ES:DI
        pop     di
        pop     cx
        add     di,RL_DEVLEN
        loop    rsi_dev
IF RL_NULDEV NE RL_LOL + LL_NUL
        .ERR    <RMDOS.INC's RL_NULDEV and RMINFO's LL_NUL disagree>
ENDIF

        ; The table DOSMGR's callout 0 is answered with: DOS 5's
        ; Win386_DOSVars, a version and six offsets in this segment.
        ; DOSMGR writes a per-machine id through User_id and leaves the
        ; rest alone for a version 5 DOS, which does its own patching
        ; - see DOS6SRC dos/dostab.asm and the callouts in i2F below.
        mov     word ptr es:[RL_DOSVARS],0005h          ; 5.0
        mov     word ptr es:[RL_DOSVARS + 2],RL_SAVEDS
        mov     word ptr es:[RL_DOSVARS + 4],RL_SAVEBX
        mov     word ptr es:[RL_DOSVARS + 6],RL_SDA + SD_INDOS
        mov     word ptr es:[RL_DOSVARS + 8],RL_USERID
        mov     word ptr es:[RL_DOSVARS + 0Ah],RL_CRITPATCH
        mov     word ptr es:[RL_DOSVARS + 0Ch],RL_UMBHEAD

        ; Every drive's path is DOS's "X:\", its root backslash at 2 and
        ; no drive parameter block until the chain below gives the ones
        ; that exist theirs, and those say so in the flags word DOS keeps
        ; there too.
        ;
        ; AND EACH STARTS WHERE THE KERNEL LEFT IT (2026-10-06).  The
        ; kernel keeps the same array at the same offset (PMDOS.INC,
        ; GCDS_) and rd_lowsave has its copy, so a drive the kernel had
        ; a volume on takes its current directory from there - the text
        ; and the cluster, which on the same disk mean the same thing.
        ; Until then every drive began at its root under Windows.
        xor     cx,cx
        mov     di,RL_CDS
        mov     si,OFFSET h_drives
rsi_cds:
        mov     al,'A'
        add     al,cl
        mov     byte ptr es:[di+CDS_TEXT],al
        mov     word ptr es:[di+CDS_TEXT+1],'\:'        ; ":\" in memory
        mov     dword ptr es:[di+CDS_DPB],0FFFFFFFFh
        mov     word ptr es:[di+CDS_BACK],2
        cmp     cx,LASTDRIVE            ; past the volumes: a structure for
        jae     rsi_cdsnext             ; a redirector, not in use
        test    byte ptr [si+RMD_FLAGS],RMDF_PRESENT
        jz      rsi_cdsnext
        mov     word ptr es:[di+CDS_FLAGS],4000h
        push    si
        mov     si,OFFSET rd_lowkeep            ; the kernel's entry, at the
        add     si,di                           ; same offset in its copy
        test    word ptr [si+CDS_FLAGS],4000h
        jz      rsi_cdsown
        cmp     word ptr [si+CDS_TEXT+1],'\:'   ; a path the kernel wrote
        jne     rsi_cdsown
        mov     eax,dword ptr [si+CDS_CLUS]
        mov     dword ptr es:[di+CDS_CLUS],eax
        push    cx
        push    di
        add     si,CDS_PATH
        add     di,CDS_PATH
        mov     cx,63
        cld
rsi_cdscpy:
        lodsb
        stosb
        or      al,al
        loopnz  rsi_cdscpy
        mov     byte ptr es:[di],0              ; ended, whatever it held
        pop     di
        pop     cx
rsi_cdsown:
        pop     si
rsi_cdsnext:
        add     si,RMD_LEN
        add     di,CDS_LEN
        inc     cx
        cmp     cl,[rd_ncds]
        jb      rsi_cds

        ; THE PARAMETER BLOCKS (2026-10-07): a chain from the list of lists
        ; of one for every drive there is, each naming rd_dskhdr as its
        ; driver and marked not read yet - rd_getvol fills one in when it
        ; mounts its drive (dpb_fill), and 32h answers with it - and every
        ; such drive's CDS pointing at its block, as DOS's do.  Windows
        ; for Workgroups' IFSMGR.386 wants a block before it will take a
        ; drive for 32-bit file access, and walks this chain itself to
        ; answer 32h for the drives it has taken (RMDISK.INC, dpb_fill).
        mov     bx,RL_LOL + LL_DPB      ; where the next link goes
        xor     cx,cx
        mov     si,OFFSET h_drives
        mov     di,RL_DPB
rsi_dpb:
        test    byte ptr [si+RMD_FLAGS],RMDF_PRESENT
        jz      rsi_dpbnext
        mov     byte ptr es:[di+DPB_DRIVE],cl
        mov     byte ptr es:[di+DPB_UNIT],cl
        mov     byte ptr es:[di+DPB_ACCESS],0FFh
        mov     word ptr es:[di+DPB_DRIVER],OFFSET rd_dskhdr
        mov     word ptr es:[di+DPB_DRIVER+2],cs
        mov     dword ptr es:[di+DPB_NEXT],0FFFFFFFFh
        mov     word ptr es:[bx],di     ; the link before it names it
        mov     word ptr es:[bx+2],es
        lea     bx,[di+DPB_NEXT]
        push    bx
        imul    bx,cx,CDS_LEN
        mov     word ptr es:[RL_CDS + bx + CDS_DPB],di
        mov     word ptr es:[RL_CDS + bx + CDS_DPB + 2],es
        pop     bx
rsi_dpbnext:
        add     si,RMD_LEN
        add     di,RL_DPBLEN
        inc     cx
        cmp     cx,LASTDRIVE
        jb      rsi_dpb

        ; THE CD-ROM, when the park brought RMCD.SYS for it (2026-10-09).
        ; Its letter becomes a redirector's, the way MSCDEX makes it one:
        ; in use, redirected - so every call naming it goes to INT 2Fh
        ; AH=11h, where RMCD answers it - and bit 7, which keeps it out
        ; of the network's list of connections, so that NET USE neither
        ; lists it nor lets it be disconnected.  Then RMCD hooks INT 2Fh.
        ; RMDOS's own vectors are in already (rd_vecs), so its hook goes
        ; on top of them.
        call    rd_cdletter             ; -> CL
        jc      rsi_nocd
        cmp     cl,[rd_ncds]
        jae     rsi_nocd
        movzx   di,cl
        imul    di,di,CDS_LEN
        add     di,RL_CDS
        mov     word ptr es:[di+CDS_FLAGS],0C080h
        mov     ax,[h_cdseg]
        push    ds
        mov     ds,ax
        mov     bx,word ptr ds:[RCH_INIT]
        pop     ds
        mov     word ptr [rd_cdinit],bx
        mov     word ptr [rd_cdinit+2],ax
        call    dword ptr [rd_cdinit]   ; ES = the block below 64K
rsi_nocd:

        ; THE NET DRIVES (BA2b): a letter the kernel had a NET USE on is a
        ; redirector's here too, marked the way the CD-ROM is - in use,
        ; redirected (so every call naming it goes to INT 2Fh AH=11h, where
        ; RMNET2.INC's server answers it through the bridge), and bit 7, so
        ; NET USE neither lists nor disconnects it.  No parameter block: it
        ; is no volume down here.  Each such letter's bit is set in rd_netmap,
        ; which the server's ownership test reads.  ES = the block below 64K.
        mov     dword ptr [rd_netmap],0
        xor     cx,cx
        mov     si,OFFSET h_drives
rsi_net:
        cmp     cl,[h_ndrv]
        jae     rsi_netdone
        test    byte ptr [si+RMD_FLAGS],RMDF_NET
        jz      rsi_netnext
        cmp     cl,[rd_ncds]
        jae     rsi_netnext
        movzx   di,cl
        imul    di,di,CDS_LEN
        add     di,RL_CDS
        mov     word ptr es:[di+CDS_FLAGS],0C080h
        mov     dword ptr es:[di+CDS_DPB],0FFFFFFFFh
        mov     eax,1
        push    cx
        and     cl,1Fh
        shl     eax,cl
        pop     cx
        or      dword ptr [rd_netmap],eax
rsi_netnext:
        add     si,RMD_LEN
        inc     cl
        jmp     short rsi_net
rsi_netdone:

        pop     es
        popa
        ret

;--- rd_cdletter - CL = the letter RMCD.SYS serves (0 = A), CF if the
;--- park brought none.  Everything else is kept.  DS must be ours.
rd_cdletter:
        push    ax
        push    ds
        mov     ax,[h_cdseg]
        or      ax,ax
        jz      rcl_none
        mov     ds,ax
        cmp     dword ptr ds:[RCH_SIG],RC_SIG
        jne     rcl_none
        mov     cl,byte ptr ds:[RCH_LETTER]
        cmp     cl,RL_NCDS
        jae     rcl_none
        pop     ds
        pop     ax
        clc
        ret
rcl_none:
        pop     ds
        pop     ax
        stc
        ret

rd_initend:                             ; the end of the start-up code
IF (rd_initend - rd_lowkeep) GT RD_KEEPLEN + RD_INITLEN
        .ERR    <RMSTK.INC: the start-up code in the slots is more than RD_INITLEN>
ENDIF
            db  RD_STKSLOTS * RD_STKSIZE - (rd_initend - rd_stks) dup(0)
rd_stktop:                              ; ...and the last at rd_stktop
