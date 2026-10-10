;=====================================================================
; RMINFO.INC - the calls that only answer, and the structures they
;              answer with
;
; Nothing here touches a disk.  What it does do is publish the shapes
; a DOS is expected to have: the swappable data area, the list of
; lists, the country block.  Windows reads all three within a second
; of starting, and a DOS that has them wrong is a DOS Windows declines
; to run on rather than one it runs on badly.
;=====================================================================

        INCLUDE RMSDA.INC               ; the SDA's offsets, shared with RMCD

;--- the list of lists, in the shape a real-mode program walks -------
LL_MCBSEG       EQU 00h                 ; dw, and it is at [ES:BX-2]
LL_PTR          EQU 02h                 ; where ES:BX points
LL_DPB          EQU 02h                 ; dd
LL_SFT          EQU 06h                 ; dd
LL_CLOCK        EQU 0Ah                 ; dd
LL_CON          EQU 0Eh                 ; dd
LL_MAXSEC       EQU 12h                 ; dw
LL_BUFFERS      EQU 14h                 ; dd
LL_CDS          EQU 18h                 ; dd
LL_FCB          EQU 1Ch                 ; dd
LL_PROTFCB      EQU 20h                 ; dw
LL_NBLOCK       EQU 22h                 ; db
LL_LASTDRV      EQU 23h                 ; db
LL_NUL          EQU 24h                 ; the NUL header, and the chain head
LL_SIZE         EQU 24h + 18 + 1

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
        mov     word ptr es:[RL_LOL + LL_SFT],OFFSET rd_sfthdr
        mov     word ptr es:[RL_LOL + LL_SFT + 2],cs    ; a chain, and a
        ; walker follows a chain without checking its head - see
        ; rd_sfthdr in RMFILE.INC for what FFFF:FFFF here cost.  And
        ; two blocks long, the second empty, because VSHARE looks one
        ; block further than the last (also at rd_sfthdr)
        mov     word ptr [rd_sfthdr],OFFSET rd_sftend
        mov     word ptr [rd_sfthdr + 2],cs
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

rd_cdinit   dd  0                       ; RMCD's cd_init, far

;=====================================================================
; rd_chain - NUL's next pointer, and with it every driver CONFIG.SYS
;            loaded.  ES = the block below 64K.  Registers preserved.
;
; THE KERNEL'S CHAIN BECOMES THIS DOS'S CHAIN.  A DEVICE= line on the
; kernel reads the driver into conventional memory, calls its INIT
; and links its header in behind NUL (DEVLOAD.INC), and all of that
; is still true after the handover: the driver is real-mode code at a
; real address, its header is a header, and its last link is the
; kernel's CON - which is RMDOS's CON now, at the same address
; (RMDOS.INC).  So NUL's next pointer is the kernel's, out of the copy
; rd_lowsave kept, and the drivers are this DOS's too.
;
; EXCEPT A DEVICE WHOSE ENTRY IS A HLT.  The kernel serves some
; devices of its own from 32-bit code (MSCD001, the CD-ROM driver
; MSCDEX would have talked to): their entries are a HLT the kernel's
; V86 monitor catches, and there is no monitor here to catch it.  They
; are taken out of the chain by linking around them, and a link made
; in somebody else's header is undone by rd_unchain on the way home.
;
; The walk stops at RMDOS's CON, at the end of a chain, or after
; RD_CHAINMAX headers, whichever comes first, and links what it has
; to CON: a chain that does not come back to CON is not one the kernel
; built, and a loop would hang every name lookup in the machine.
; RD_CHAINMAX and RD_UNLINKMAX are in RMDEV.INC, which drv_find shares
; the first with and which is included ahead of this file.
;=====================================================================
rd_chain:
        pusha
        push    ds
        push    fs
        mov     word ptr [rd_unlinkn],0
        mov     word ptr [rd_ndrv],0
        mov     ax,es
        mov     fs,ax                           ; FS:BX = the link to write,
        mov     bx,RL_NULDEV                    ; NUL's to start with
        mov     si,word ptr [rd_lowkeep + RL_NULDEV]
        mov     dx,word ptr [rd_lowkeep + RL_NULDEV + 2]
        mov     cx,RD_CHAINMAX
rch_one:
        cmp     si,0FFFFh
        je      rch_con                         ; the chain ended early
        cmp     si,RL_DEVS
        jne     rch_driver
        cmp     dx,[rd_lowseg]
        je      rch_con                         ; home: the kernel's CON
rch_driver:
        push    ds
        mov     ds,dx
        mov     di,word ptr [si+6]              ; its strategy entry
        cmp     byte ptr [di],0F4h              ; a HLT?
        mov     ax,word ptr [si]                ; and where it goes next
        mov     di,word ptr [si+2]
        pop     ds
        je      rch_skip
        ; a driver: linked from FS:BX, and its own link is the next
        ; one to write
        mov     word ptr fs:[bx],si
        mov     word ptr fs:[bx+2],dx
        inc     word ptr [rd_ndrv]
        mov     bx,si
        mov     fs,dx
        mov     si,ax
        mov     dx,di
        loop    rch_one
        jmp     short rch_con
rch_skip:
        ; LEFT OUT: FS:BX keeps pointing at the link before it.  If that
        ; link is in a driver's header rather than in this block, what
        ; it held is noted so that rd_unchain can put it back.
        call    rch_note
        mov     si,ax
        mov     dx,di
        loop    rch_one
rch_con:
        call    rch_note
        mov     word ptr fs:[bx],RL_DEVS
        mov     ax,[rd_lowseg]
        mov     word ptr fs:[bx+2],ax
        pop     fs
        pop     ds
        popa
        ret

;--- rch_note - FS:BX is a link about to be written.  If it is outside
;--- the block below 64K, keep its address and what it holds now.
rch_note:
        push    ax
        push    si
        mov     ax,fs
        cmp     ax,[rd_lowseg]
        je      rcn_out                         ; the kernel puts that back
        mov     si,[rd_unlinkn]
        cmp     si,RD_UNLINKMAX
        jae     rcn_out
        imul    si,si,8
        add     si,OFFSET rd_unlinks
        ; already noted?  The end link is written after the last skip
        ; and may be the same field.
        push    di
        mov     di,OFFSET rd_unlinks
rcn_seen:
        cmp     di,si
        jae     rcn_new
        cmp     word ptr [di],bx
        jne     rcn_next
        cmp     word ptr [di+2],ax
        je      rcn_had
rcn_next:
        add     di,8
        jmp     short rcn_seen
rcn_had:
        pop     di
        jmp     short rcn_out
rcn_new:
        pop     di
        mov     word ptr [si],bx
        mov     word ptr [si+2],ax
        mov     ax,word ptr fs:[bx]
        mov     word ptr [si+4],ax
        mov     ax,word ptr fs:[bx+2]
        mov     word ptr [si+6],ax
        inc     word ptr [rd_unlinkn]
rcn_out:
        pop     si
        pop     ax
        ret

;=====================================================================
; rd_unchain - put back every link rd_chain wrote in a driver's own
;              header.  Called on the way home, before rd_lowrest puts
;              the block itself back.
;=====================================================================
rd_unchain:
        pusha
        push    es
        mov     cx,[rd_unlinkn]
        mov     si,OFFSET rd_unlinks
ruc_one:
        jcxz    ruc_out
        les     di,dword ptr [si]
        mov     ax,word ptr [si+4]
        mov     word ptr es:[di],ax
        mov     ax,word ptr [si+6]
        mov     word ptr es:[di+2],ax
        add     si,8
        dec     cx
        jmp     short ruc_one
ruc_out:
        mov     word ptr [rd_unlinkn],0
        pop     es
        popa
        ret

rd_ndrv     dw  0                       ; drivers in the chain, for the trace
rd_unlinkn  dw  0
rd_unlinks  dw  RD_UNLINKMAX * 4 dup (0)        ; far link, then what it held

rsi_devtab:
        dw  8013h                       ; CON: character, stdin, stdout, and
        db  'CON     '                  ; INT 29h is served here (i29)
        dw  8000h
        db  'AUX     '
        dw  8000h
        db  'PRN     '

;=====================================================================
; 2Ah - the date -> CX = year, DH = month, DL = day, AL = weekday
; 2Ch - the time -> CH:CL = hours:minutes, DH:DL = seconds:hundredths
;
; Both out of the CMOS through the BIOS, because there is no clock
; driver here and inventing one would mean keeping a tick count that
; nothing increments.
;=====================================================================
fn2A:
        mov     ah,04h
        int     1Ah                     ; CH=century CL=year DH=month DL=day
        jc      f2A_none
        mov     al,ch
        call    bcd2bin
        mov     bl,al
        mov     al,cl
        call    bcd2bin
        mov     bh,al
        movzx   ax,bl
        ; THE MONTH AND THE DAY ARE IN DX, AND MUL WRITES DX.  Until
        ; 2026-10-05 the year was made with "mul cx", which left DX as
        ; the high word of the product - zero - and every program under
        ; Windows was told the date was 2026-00-00.  Monopoly Deluxe's C
        ; runtime turned that into a day of the year of -1 and searched
        ; its months table for a month that holds it, for ever; that
        ; was "WinExec never returns" in the enhanced-mode sweep.  The
        ; kernel's own 2Ah never had this; this file's did.
        imul    ax,ax,100               ; no DX
        movzx   bx,bh
        add     ax,bx
        mov     [bp+F_CX],ax
        mov     al,dh
        call    bcd2bin
        mov     byte ptr [bp+F_DX+1],al
        mov     dh,al                   ; DH = month, binary
        mov     al,dl
        call    bcd2bin
        mov     byte ptr [bp+F_DX],al
        mov     dl,al                   ; DL = day, binary
        mov     cx,[bp+F_CX]
        call    rd_weekday              ; CX year, DH month, DL day -> AL
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok
f2A_none:
        mov     word ptr [bp+F_CX],1980
        mov     word ptr [bp+F_DX],0101h
        mov     byte ptr [bp+F_AX],2
        jmp     i21_ok

;--- rd_weekday - CX = year, DH = month (1-12), DL = day -> AL = the day
;--- of the week, 0 Sunday, as 2Ah answers it.  Sakamoto's method: the
;--- year is stepped back one for January and February, and the month
;--- gets its offset from a twelve-byte table.  Only AL comes back
;--- changed.
rd_weekday:
        push    bx
        push    cx
        push    dx
        cmp     dh,3
        jae     rwd_year
        dec     cx                      ; January and February belong to
rwd_year:                               ; the year before, for this
        movzx   bx,dh
        dec     bx
        and     bx,0Fh                  ; a month out of range reads a
        cmp     bx,12                   ; harmless 0
        jb      rwd_mon
        xor     bx,bx
rwd_mon:
        mov     bl,[rwd_tab + bx]
        movzx   ax,dl                   ; day
        add     ax,bx                   ; + month offset
        add     ax,cx                   ; + y
        mov     bx,cx
        shr     bx,2
        add     ax,bx                   ; + y/4
        push    ax
        mov     ax,cx
        xor     dx,dx
        mov     bx,100
        div     bx                      ; AX = y/100
        mov     bx,ax
        pop     ax
        sub     ax,bx                   ; - y/100
        shr     bx,2
        add     ax,bx                   ; + y/400
        xor     dx,dx
        mov     bx,7
        div     bx                      ; DX = the remainder
        mov     al,dl
        pop     dx
        pop     cx
        pop     bx
        ret
rwd_tab     db  0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4

fn2C:
        mov     ah,02h
        int     1Ah                     ; CH=hours CL=minutes DH=seconds
        jc      f2C_none
        mov     al,ch
        call    bcd2bin
        mov     byte ptr [bp+F_CX+1],al
        mov     al,cl
        call    bcd2bin
        mov     byte ptr [bp+F_CX],al
        mov     al,dh
        call    bcd2bin
        mov     byte ptr [bp+F_DX+1],al
        mov     byte ptr [bp+F_DX],0
        jmp     i21_ok
f2C_none:
        mov     word ptr [bp+F_CX],0
        mov     word ptr [bp+F_DX],0
        jmp     i21_ok

;--- 2Bh set date, 2Dh set time.  Refused rather than pretended: the
;--- excursion is minutes long and a program that sets the clock here
;--- and finds it unset afterwards is worse off than one told no.
fn2B:
fn2D:
        mov     byte ptr [bp+F_AX],0FFh
        jmp     i21_ok

bcd2bin:
        push    cx
        mov     ah,al
        and     al,0Fh
        shr     ah,4
        mov     cl,10
        push    ax
        mov     al,ah
        mul     cl
        pop     cx
        and     cx,0Fh
        add     al,cl
        pop     cx
        ret


;=====================================================================
; 2Eh - set the verify flag.  AL = 0 off, 1 on.
;
; Remembered and reported and NOT acted on, which is what it amounts
; to here: verifying a write means reading the sector back, and every
; write in this DOS already goes through one BIOS call per sector on a
; path that runs for minutes.  A program that turns it on and reads it
; back through 54h gets a consistent answer.
;=====================================================================
fn2E:
        mov     al,byte ptr [bp+F_AX]
        mov     [rd_verify],al
        jmp     i21_ok
;=====================================================================
; 25h - set the vector in AL to DS:DX
; 35h - and read it back into ES:BX
;=====================================================================
fn25:
        push    es
        movzx   di,byte ptr [bp+F_AX]
        shl     di,2
        xor     ax,ax
        mov     es,ax
        mov     ax,[bp+F_DX]
        cli
        mov     es:[di],ax
        mov     ax,[bp+F_DS]
        mov     es:[di+2],ax
        sti
        pop     es
        jmp     i21_ok

fn35:
        push    es
        movzx   di,byte ptr [bp+F_AX]
        shl     di,2
        xor     ax,ax
        mov     es,ax
        mov     ax,es:[di]
        mov     [bp+F_BX],ax
        mov     ax,es:[di+2]
        mov     [bp+F_ES],ax
        pop     es
        jmp     i21_ok

;=====================================================================
; 30h - the version.  Six point twenty-two, which is what PM-DOS says
;       and what everything this is standing in for was written
;       against.  BH is the OEM; FFh is "not one of the ones with a
;       name", which is the truthful answer.
;=====================================================================
fn30:
        mov     al,byte ptr [bp+F_AX]
        mov     word ptr [bp+F_AX],DOSVER_MINOR * 256 + DOSVER_MAJOR
        mov     word ptr [bp+F_BX],0FF00h
        cmp     al,1
        jne     f30_oem
        ; AL=1 asks for the version FLAG in BH rather than the OEM:
        ; 08h for a DOS in ROM, 10h for one in the HMA, and this one is
        ; neither.  DOSMGR asks, and read the OEM byte as "in ROM".
        mov     word ptr [bp+F_BX],0
f30_oem:
        mov     word ptr [bp+F_CX],0
        jmp     i21_ok

;=====================================================================
; 33h - the odds and ends DOS filed under one number
;=====================================================================
fn33:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jz      f33_get
        cmp     al,1
        je      f33_set
        cmp     al,5
        je      f33_boot
        cmp     al,6
        je      f33_true
        jmp     i21_unsup
f33_get:
        mov     al,[rd_brkchk]
        mov     byte ptr [bp+F_DX],al
        jmp     i21_ok
f33_set:
        mov     al,byte ptr [bp+F_DX]
        mov     [rd_brkchk],al
        jmp     i21_ok
f33_boot:
        mov     al,[h_bootdrv]
        inc     al                      ; 33h/05h counts A as one
        mov     byte ptr [bp+F_DX],al
        jmp     i21_ok
f33_true:
        mov     word ptr [bp+F_BX],DOSVER_MINOR * 256 + DOSVER_MAJOR
        mov     word ptr [bp+F_DX],0
        jmp     i21_ok

;=====================================================================
; 34h - the address of the InDOS flag.
;
; And it is the SECOND BYTE OF THE SWAPPABLE DATA AREA, exactly as it
; is in the kernel this is standing in for, so a TSR that finds the
; critical-error flag by subtracting one gets the right byte.
;=====================================================================
fn34:
        mov     ax,[rd_lowseg]
        mov     [bp+F_ES],ax
        mov     word ptr [bp+F_BX],RL_SDA + SD_INDOS
        jmp     i21_ok

;=====================================================================
; 37h - the switch character.  A slash, and it cannot be set.
;=====================================================================
fn37:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jnz     f37_other
        mov     byte ptr [bp+F_DX],'/'
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok
f37_other:
        cmp     al,1
        je      f37_ok
        jmp     i21_unsup
f37_ok:
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok

;=====================================================================
; 38h - country information.  One country, the United States, which is
;       the one PM-DOS answers too.
;=====================================================================
fn38:
        mov     ax,[bp+F_DX]
        cmp     ax,0FFFFh
        je      f38_set
        push    es
        mov     es,[bp+F_DS]
        mov     di,ax
        mov     si,OFFSET rd_country
        mov     cx,34
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds
        pop     es
        mov     word ptr [bp+F_BX],1    ; the country code
        jmp     i21_ok
f38_set:
        jmp     i21_ok                  ; there is only one to set it to

;=====================================================================
; 52h - the list of lists.  ES:BX, and the word BELOW it is the first
;       memory control block, which is the one field here that has
;       meant the same thing in every DOS since 2.0 and the one a
;       memory tool is really after.
;=====================================================================
fn52:
        mov     ax,[rd_lowseg]
        mov     [bp+F_ES],ax
        mov     word ptr [bp+F_BX],RL_LOL + LL_PTR
        jmp     i21_ok

;=====================================================================
; 54h - the verify flag
;=====================================================================
fn54:
        mov     al,[rd_verify]
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; 59h - the last error, with its class, action and locus.
;
; Derived from the code rather than stored, exactly as the kernel does
; it - which is why there is a table here and not three variables.
;=====================================================================
fn59:
        mov     ax,[rd_lasterr]
        mov     [bp+F_AX],ax
        mov     si,OFFSET rd_errtab
f59_scan:
        movzx   cx,byte ptr [si]
        or      cx,cx
        jz      f59_unknown
        cmp     cx,ax
        je      f59_have
        add     si,4
        jmp     short f59_scan
f59_unknown:
        mov     si,OFFSET rd_errunk
f59_have:
        mov     bh,[si+1]
        mov     bl,[si+2]
        mov     [bp+F_BX],bx
        mov     ch,[si+3]
        mov     byte ptr [bp+F_CX+1],ch
        jmp     i21_ok

rd_errtab:
        db  ERR_FUNC,      07h, 05h, 01h
        db  ERR_NOFILE,    08h, 03h, 02h
        db  ERR_NOPATH,    08h, 03h, 02h
        db  ERR_NOHANDLES, 01h, 04h, 01h
        db  ERR_ACCESS,    03h, 04h, 02h
        db  ERR_BADHANDLE, 07h, 04h, 01h
        db  ERR_BADMCB,    04h, 05h, 05h
        db  ERR_NOMEM,     01h, 04h, 05h
        db  ERR_BADBLOCK,  07h, 05h, 05h
        db  ERR_EXEFMT,    09h, 04h, 01h
        db  ERR_BADDRIVE,  08h, 03h, 02h
        db  ERR_EXISTS,    0Ch, 04h, 02h
        db  0
rd_errunk:
        db  0, 0Dh, 07h, 01h

;=====================================================================
; 5Dh - and the one subfunction of it that matters here.
;
;   AL=06h  the swappable data area -> DS:SI, CX its length, DX how
;           much of it must always be swapped
;
; The consumer this exists for is Windows' own DOSMGR, which asks for
; this within moments of starting so it can give every virtual machine
; its own copy of DOS's per-process state.  A DOS that cannot answer
; is a DOS Windows will not instance, and one DOS box's current
; directory becomes every DOS box's current directory.
;=====================================================================
fn5D:
        mov     al,byte ptr [bp+F_AX]
        cmp     al,06h
        je      f5D_sda
        jmp     i21_unsup
f5D_sda:
        call    sda_sync
        mov     ax,[rd_lowseg]
        mov     [bp+F_DS],ax
        mov     word ptr [bp+F_SI],RL_SDA
        mov     word ptr [bp+F_CX],SD_LEN
        mov     word ptr [bp+F_DX],SD_LEN
        jmp     i21_ok

;=====================================================================
; 5Eh - the machine name.  AL=0: copy the sixteen bytes to DS:DX, CH =
; 1 once a name has been set (0 before: there is no name), CL = the
; NetBIOS name number.  AL=1: take the sixteen bytes at DS:DX and the
; number in CX.  MS-DOS keeps these itself ($UserOper, DOS6SRC
; dos/macro.asm: "MSDOS will only maintain this string and do no
; verifications"), and WfW's VNETSUP.386 SETS the name as the network
; starts - 5E01h, which this DOS refused with error 1 until 2026-10-09
; (the floppy boot on real hardware: Windows up to its start-up sound
; with the network, fine without it).  Fifteen bytes and a NUL, and CX
; back as 5E01h left it - CH nonzero for a name that is set is the
; setter's convention, not DOS's (UserGet, UserSet).  AL=2-5 are the
; redirected printer's set-up string and flags, which MS-DOS passes to
; the redirector as INT 2Fh 111Fh, and so does this (net_user).
;=====================================================================
fn5E:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jz      f5E_get
        cmp     al,1
        je      f5E_set
        cmp     al,5
        ja      f5E_bad
        mov     ax,111Fh                ; 02h-05h: the redirector's
        jmp     net_user
f5E_bad:
        jmp     i21_unsup
f5E_get:
        push    ds
        push    es
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        mov     si,OFFSET rd_myname
        mov     cx,15
        cld
        rep     movsb
        xor     al,al
        stosb
        pop     es
        pop     ds
        mov     cx,[rd_mynum]
        mov     [bp+F_CX],cx
        jmp     i21_ok
f5E_set:
        push    ds
        push    es
        push    ds
        pop     es
        mov     ds,[bp+F_DS]
        mov     si,[bp+F_DX]
        mov     di,OFFSET rd_myname
        mov     cx,15
        cld
        rep     movsb
        pop     es
        pop     ds
        mov     ax,[bp+F_CX]
        mov     [rd_mynum],ax
        mov     byte ptr [rd_difnam],1
        jmp     i21_ok

rd_myname   db  15 dup(' '), 0          ; the machine name, as 5E01h set it
rd_mynum    dw  0                       ; its NetBIOS name number
rd_difnam   db  0                       ; 1 once 5E01h has been called

;=====================================================================
; 5Fh - the redirection list.  07h and 08h are DOS's own: DL = a drive
; (0 = A), whose current directory structure is marked in use or not
; - a drive with no parameter block behind it is refused, as DOS
; refuses one with no device pointer.  Everything else - the assign
; mode, the list, redirect, cancel - is the redirector's, and goes to
; it as INT 2Fh 111Eh with the caller's own registers, as MS-DOS's
; $AssignOper sends it (DOS6SRC dos/macro.asm).  A NET USE in a box is
; 5F03h.  It was refused here until 2026-10-09, whoever was listening.
;=====================================================================
fn5F:
        mov     al,byte ptr [bp+F_AX]
        cmp     al,07h
        je      f5F_inuse
        cmp     al,08h
        je      f5F_inuse
        mov     ax,111Eh
        jmp     net_user
f5F_inuse:
        mov     al,byte ptr [bp+F_DX]
        cmp     al,[rd_ncds]
        jae     f5F_bad
        push    es
        mov     es,[rd_lowseg]
        movzx   di,al
        imul    di,di,CDS_LEN
        add     di,RL_CDS
        mov     ax,word ptr es:[di+CDS_DPB]
        or      ax,ax
        jz      f5F_nodpb
        cmp     ax,0FFFFh
        je      f5F_nodpb
        cmp     byte ptr [bp+F_AX],07h
        jne     f5F_reset
        or      byte ptr es:[di+CDS_FLAGS+1],40h
        pop     es
        jmp     i21_ok
f5F_reset:
        and     byte ptr es:[di+CDS_FLAGS+1],NOT 40h
        pop     es
        jmp     i21_ok
f5F_nodpb:
        pop     es
f5F_bad:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;=====================================================================
; sda_sync - this file's variables into the published SDA.
; sda_pull - and the published SDA back into them.
;
; THE PUBLISHED COPY IS THE ONE THAT IS INSTANCED, and that is the
; whole reason there are two.  DOSMGR gives every virtual machine its
; own copy of the bytes 5D06h names and swaps them as it switches, so
; the current process, DTA, drive, error and InDOS count that belong
; to a call are the ones it swapped in before the call was made.  Every
; INT 21h therefore starts by pulling them (RMDOS.ASM, i21) and ends by
; putting them back (i21_out), and the two paths that leave DOS without
; passing i21_out - a process ending, EXEC starting one - put them
; back themselves.  Between the two the variables are the truth and
; the published copy is stale, which is what MS-DOS's own "swap only
; when InDOS is zero" rule means.
;
; The one place DOS gives the machine away while still inside a call
; is the console's wait for a key, which calls INT 28h so that Windows
; can run another machine meanwhile; RMCON.INC syncs before and pulls
; after, for the same reason.  Both preserve every register: they are
; called from the INT 21h prologue with the caller's frame address in
; CX and DX, and from code that has no registers to spare.
;=====================================================================
sda_sync:
        push    ax
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     ax,word ptr cs:[rd_psp]
        mov     word ptr es:[RL_SDA + SD_PSP],ax
        mov     ax,word ptr cs:[rd_dtaoff]
        mov     word ptr es:[RL_SDA + SD_DTA],ax
        mov     ax,word ptr cs:[rd_dtaseg]
        mov     word ptr es:[RL_SDA + SD_DTA + 2],ax
        mov     al,byte ptr cs:[rd_drive]
        mov     byte ptr es:[RL_SDA + SD_DRIVE],al
        mov     ax,word ptr cs:[rd_lasterr]
        mov     word ptr es:[RL_SDA + SD_EXTERR],ax
        mov     ax,word ptr cs:[rd_exitcode]
        mov     word ptr es:[RL_SDA + SD_EXITCODE],ax
        mov     al,byte ptr cs:[rd_indos]
        mov     byte ptr es:[RL_SDA + SD_INDOS],al
        mov     al,byte ptr cs:[rd_errmode]
        mov     byte ptr es:[RL_SDA + SD_ERRMODE],al
        pop     es
        pop     ax
        ret

;--- sda_userax - AX = the caller's AX, into the SDA's USER_IN_AX, where
;--- MS-DOS's dispatcher leaves it on every call and a redirector looks
;--- to see which call one of its own is part of.
sda_userax:
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     word ptr es:[RL_SDA + SD_USERAX],ax
        pop     es
        ret

sda_pull:
        push    ax
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     ax,word ptr es:[RL_SDA + SD_PSP]
        mov     word ptr cs:[rd_psp],ax
        mov     ax,word ptr es:[RL_SDA + SD_DTA]
        mov     word ptr cs:[rd_dtaoff],ax
        mov     ax,word ptr es:[RL_SDA + SD_DTA + 2]
        mov     word ptr cs:[rd_dtaseg],ax
        mov     al,byte ptr es:[RL_SDA + SD_DRIVE]
        mov     byte ptr cs:[rd_drive],al
        mov     ax,word ptr es:[RL_SDA + SD_EXTERR]
        mov     word ptr cs:[rd_lasterr],ax
        mov     ax,word ptr es:[RL_SDA + SD_EXITCODE]
        mov     word ptr cs:[rd_exitcode],ax
        mov     al,byte ptr es:[RL_SDA + SD_INDOS]
        mov     byte ptr cs:[rd_indos],al
        mov     al,byte ptr es:[RL_SDA + SD_ERRMODE]
        mov     byte ptr cs:[rd_errmode],al
        pop     es
        pop     ax
        ret

;=====================================================================
; 63h - the double-byte character set table.  There is none, and a
;       pointer to a zero word is how DOS says so.
;=====================================================================
fn63:
        mov     ax,cs
        mov     [bp+F_DS],ax
        mov     word ptr [bp+F_SI],OFFSET rd_dbcs
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok

;=====================================================================
; 65h - extended country information: the block, the subfunctions that
;       do something, and the ones that hand back a table.
;
; THE TABLES ARE THE KERNEL'S, where it built them in the DOS data
; segment (GNLS_ in PMDOS.INC), which this DOS leaves alone (RL_NLS).
; They were refused here until 2026-10-06, and Windows for Workgroups
; 3.11's IFSMGR.386 asks for 6504h - the file name upper-case table -
; as it starts.  The answer is the kernel's: five bytes at ES:DI, the
; subfunction and a far pointer, and CX = 5.
;=====================================================================
F65_INFOLEN     EQU     7 + 34          ; 6501h: header, then 38h's block

fn65:
        mov     al,byte ptr [bp+F_AX]
        mov     dx,RL_NLSUP
        cmp     al,02h
        je      f65_table
        cmp     al,04h                  ; file names: the same table
        je      f65_table
        mov     dx,RL_NLSTERM
        cmp     al,05h
        je      f65_table
        mov     dx,RL_NLSCOLL
        cmp     al,06h
        je      f65_table
        mov     dx,RL_NLSDBCS
        cmp     al,07h
        je      f65_table
        cmp     al,20h
        je      f65_upchar
        cmp     al,21h
        je      f65_upmem
        cmp     al,22h
        je      f65_upasciz
        cmp     al,23h
        je      f65_yesno
        cmp     al,01h
        je      f65_info
        jmp     i21_unsup

;--- 6501h: the extended country block - ID 1, its length, country,
;--- code page, then the same 34 bytes 38h hands back.  ES:DI -> the
;--- caller's buffer, CX its size; as much as fits, and CX says how
;--- much that was.
f65_info:
        mov     cx,[bp+F_CX]
        cmp     cx,5
        jb      f65_short
        cmp     cx,F65_INFOLEN
        jbe     f65_fits
        mov     cx,F65_INFOLEN
f65_fits:
        mov     [bp+F_CX],cx
        push    es
        mov     es,[bp+F_ES]
        mov     di,[bp+F_DI]
        mov     si,OFFSET f65_hdr
        cld
        push    cx
        mov     cx,7
        rep     movsb                   ; ID, length, country, code page
        pop     cx
        sub     cx,7
        jbe     f65_infodone
        mov     si,OFFSET rd_country
        rep     movsb
f65_infodone:
        pop     es
        jmp     i21_ok
f65_short:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;--- 6502h, 6504h-6507h: AL = which, DX = the table's offset ---------
f65_table:
        cmp     word ptr [bp+F_CX],5
        jb      f65_short
        push    es
        mov     es,[bp+F_ES]
        mov     di,[bp+F_DI]
        mov     es:[di],al
        mov     es:[di+1],dx
        mov     ax,[rd_lowseg]
        mov     es:[di+3],ax
        pop     es
        mov     word ptr [bp+F_CX],5
        jmp     i21_ok

;--- 6523h: is the character in DL a yes or a no?  AX = 1 yes, 0 no,
;--- 2 neither.  Y and N, in either case: this is the one country.
f65_yesno:
        mov     al,byte ptr [bp+F_DX]
        call    rd_upcase
        mov     word ptr [bp+F_AX],1
        cmp     al,'Y'
        je      f65_ynout
        mov     word ptr [bp+F_AX],0
        cmp     al,'N'
        je      f65_ynout
        mov     word ptr [bp+F_AX],2
f65_ynout:
        jmp     i21_ok
f65_upchar:
        mov     al,byte ptr [bp+F_DX]
        call    rd_upcase
        mov     byte ptr [bp+F_DX],al
        jmp     i21_ok
f65_upmem:
        push    es
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        mov     cx,[bp+F_CX]
        jcxz    f65_memdone
f65_memloop:
        mov     al,es:[di]
        call    rd_upcase
        mov     es:[di],al
        inc     di
        loop    f65_memloop
f65_memdone:
        pop     es
        jmp     i21_ok
f65_upasciz:
        push    es
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
f65_azloop:
        mov     al,es:[di]
        or      al,al
        jz      f65_azdone
        call    rd_upcase
        mov     es:[di],al
        inc     di
        jmp     short f65_azloop
f65_azdone:
        pop     es
        jmp     i21_ok

f65_hdr     db  1                       ; info ID
            dw  F65_INFOLEN - 3         ; the length after this word
            dw  1                       ; country: the United States
            dw  437                     ; code page

;=====================================================================
; 66h - the global code page.  437 is the only one there is: 6601h
; says so for both the active and the system page, and 6602h accepts
; 437 and refuses anything else, as DOS does for a page it has no
; tables for.
;=====================================================================
fn66:
        mov     al,byte ptr [bp+F_AX]
        cmp     al,1
        je      f66_get
        cmp     al,2
        jne     f66_fn
        cmp     word ptr [bp+F_BX],437
        jne     f66_bad
        jmp     i21_ok
f66_get:
        mov     word ptr [bp+F_BX],437
        mov     word ptr [bp+F_DX],437
        jmp     i21_ok
f66_bad:
        mov     ax,ERR_NOFILE           ; what DOS says when it cannot load it
        jmp     i21_fail
f66_fn:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; INT 24h - the default critical error handler.
;
; It FAILS the call rather than asking.  A handler that prompted would
; be writing "Abort, Retry, Fail?" onto a screen that belongs to
; Windows, in a video mode that may not have text in it, and waiting
; for a keystroke from a keyboard Windows has taken over.  A program
; with its own handler installs one and never reaches this.
;=====================================================================
i24:
        mov     al,3                    ; fail
        iret

;=====================================================================
; INT 2Fh - the multiplex.
;
; AH=43h IS THE ONE THAT MATTERS.  Windows in 386 enhanced mode does
; not ask whether there is extended memory - it requires a driver, and
; AL=00h answering anything but 80h ends the conversation.  It answers
; 80h now, and AL=10h hands back the far pointer every XMS client
; calls from then on.  The driver behind it is RMXMS.INC.
;=====================================================================
i2F:
IFDEF DBG_LPT
        call    rdbg_i2frec
ENDIF
IFDEF RMDOS_E9
        call    e9_i2f
ENDIF
        cmp     ah,11h
        je      i2F_net                 ; the network redirector: none
        cmp     ah,13h
        je      i2F_disk13              ; IO.SYS's Orig13 and Old13
        cmp     ah,43h
        je      i2F_xms
        cmp     ax,1600h
        je      i2F_nowin
        cmp     ax,1000h
        je      i2F_share
        cmp     ax,4A10h
        je      i2F_bambi
        cmp     ax,1606h
        je      i2F_winexit
        cmp     ax,1607h
        je      i2F_devcall
        cmp     ax,160Ah
        je      i2F_iret
        cmp     ax,4680h
        je      i2F_iret
        cmp     ah,12h
        je      i2F_none                ; DOS's own internal services
        iret

;---------------------------------------------------------------------
; 1607h - a virtual device calling out to the DOS.  BX names the
; device, and the only one a DOS answers is DOSMGR (0015h); CX is
; the question.  The answers are MS-DOS 5's, from DOS6SRC
; dos/mscode.asm, Win386_Query - including the two magic numbers
; AX=B97Ch and DX=A2ABh that mean "done" - because DOSMGR reads
; them as "this DOS knows about Windows" and does less guessing.
;
;   CX=0  is this DOS Windows-aware?  CX=1, ES:BX -> the variable
;         table (RL_DOSVARS)
;   CX=1  apply the patches in DX: all of them "done", as DOS 5 says,
;         since a version 5 table means DOSMGR does no patching
;   CX=2  remove them: nothing to do
;   CX=3  the size of a structure: DX bit 0 is the CDS, and the
;         answer is why the CDS can be our own shape
;   CX=4  which structures DOS instanced itself: none, DX=0
;   CX=5  the size of a device driver: not known, AX=DX=0
;---------------------------------------------------------------------
i2F_devcall:
        cmp     bx,0015h
        jne     i2F_iret
        or      cx,cx
        jnz     i2F_dmfn
        inc     cx
        mov     es,word ptr cs:[rd_lowseg]
        mov     bx,RL_DOSVARS
        iret
i2F_dmfn:
        cmp     cx,1
        jne     i2F_dm3
        mov     bx,dx
        jmp     short i2F_dmdone
i2F_dm3:
        cmp     cx,3
        jne     i2F_dm4
        test    dx,1
        jz      i2F_iret
        mov     cx,CDS_LEN
        jmp     short i2F_dmdone
i2F_dm4:
        cmp     cx,4
        jne     i2F_dm5
        xor     dx,dx
        iret
i2F_dm5:
        cmp     cx,5
        jne     i2F_iret
        ; ES:DI is the device header DOSMGR is about to instance.  MS-DOS
        ; sizes a driver from the 'D' arena in front of it and answers
        ; 0 - "use your own methods" - when there is none.  RMDOS's own
        ; four headers (NUL, CON, AUX, PRN) live in the borrowed DOS data
        ; block, with no arena and no driver behind them, and DOSMGR's
        ; own method got PRN wrong: Windows for Workgroups 3.11's VPD
        ; instances PRN as it starts (DOSMGR_Instance_Device on LPT1-3
        ; and PRN), and Windows then never came back from its 1608h
        ; broadcast (2026-10-06).  So for those four the answer is
        ; exact: the header is the whole driver, RL_DEVLEN bytes, in
        ; BX:CX, with the "done" codes.
        ;
        ; A DRIVER CONFIG.SYS LOADED is sized from its memory block.
        ; MS-DOS reads the arena header in the paragraph in front of
        ; the device header and takes its size if it is a 'D' arena
        ; owned by the driver's segment; the kernel's loader gives every
        ; driver a block of its own instead, owned by DOS (8), with the
        ; driver at its start (DEVLOAD.INC).  So an 'M' or 'Z' header
        ; owned by 8 in front of the driver's segment is the size, from
        ; the device header to the block's end.  Anything else still
        ; gets the 0 MS-DOS gives when it cannot tell.
        push    ax
        mov     ax,es
        cmp     ax,word ptr cs:[rd_lowseg]
        pop     ax
        jne     i2F_dm5drv
        cmp     di,RL_NULDEV
        je      i2F_dm5ours
        cmp     di,RL_DEVS
        jb      i2F_dm5no
        cmp     di,RL_DEVEND
        jae     i2F_dm5no
i2F_dm5ours:
        xor     bx,bx
        mov     cx,RL_DEVLEN
        jmp     short i2F_dmdone
i2F_dm5drv:
        push    ds
        push    ax
        mov     ax,es
        dec     ax
        mov     ds,ax                   ; DS:0 = the header in front
        cmp     byte ptr ds:[0],'M'
        je      i2F_dm5mcb
        cmp     byte ptr ds:[0],'Z'
        jne     i2F_dm5nod
i2F_dm5mcb:
        cmp     word ptr ds:[1],8       ; owned by DOS: the loader's
        jne     i2F_dm5nod
        mov     ax,word ptr ds:[3]      ; paragraphs
        mov     cx,16
        mul     cx                      ; DX:AX = the block in bytes
        sub     ax,di
        sbb     dx,0                    ; ...from the header to its end
        jc      i2F_dm5nod
        mov     cx,ax
        mov     bx,dx
        pop     ax
        pop     ds
        jmp     short i2F_dmdone
i2F_dm5nod:
        pop     ax
        pop     ds
i2F_dm5no:
        xor     ax,ax
        xor     dx,dx
        iret
i2F_dmdone:
        mov     ax,0B97Ch
        mov     dx,0A2ABh
        iret
i2F_none:
        mov     al,0
        iret

;--- 11h: the network redirector - none, if the call has got this far.
;--- AL=00h, "is one installed?", is AL=0: no.  Anything else is a
;--- request this DOS made for a drive or a file that somebody marked
;--- as a redirector's and then did not answer for (RMNET.INC), and
;--- MS-DOS's own handler fails it: AX = 1 and the carry, with the flags
;--- the INT pushed thrown away (DOS6SRC dos/mscode.asm, INT2F's
;--- BadFunc).  Answered AL=0 and no carry, as it was until the
;--- redirector interface, a request would read as done.
i2F_net:
        or      al,al
        jz      i2F_iret
        mov     ax,ERR_FUNC
        stc
        retf    2

;--- 13h: DOS 3.2's "set disk interrupt handler".  DS:DX is a new
;--- Orig13, the handler this DOS's INT 13h hook goes on to (i13 in
;--- RMDOS.ASM), and ES:BX a new Old13, the vector a reboot would put
;--- back; each is swapped for what was there, which comes back in the
;--- same registers - IO.SYS's own int2f_replace_int13, in DOS6SRC
;--- bios/msint13.asm.
;---
;--- WINDOWS ASKS IT TWICE AS IT STARTS.  WDCTRL, 32-bit disk access,
;--- reads a DS that comes back unchanged as a DOS older than 3.2 and
;--- will not run ("requires MS-DOS version 3.10 or later"); Windows for
;--- Workgroups' BLOCKDEV.386 puts a handler of its own in as Orig13
;--- and tells INT13.386 that DOS's disk calls now come through it.
;--- They do: every INT 13h this DOS makes goes through i13.  Answered
;--- on 2026-10-07 without that hook, BLOCKDEV's word was not true, and
;--- a DOS box under Windows for Workgroups died with "invalid
;--- instruction"; the answer waited for the hook (NOTES.TXT).
i2F_disk13:
        push    ax
        mov     ax,ds
        xchg    dx,word ptr cs:[rd_orig13]
        xchg    ax,word ptr cs:[rd_orig13+2]
        mov     ds,ax
        xchg    bx,word ptr cs:[rd_old13]
        mov     ax,es
        xchg    ax,word ptr cs:[rd_old13+2]
        mov     es,ax
        pop     ax
        iret
i2F_xms:
        cmp     al,00h
        je      i2F_xmshere
        cmp     al,10h
        je      i2F_xmsaddr
        iret
i2F_xmshere:
        mov     al,80h                  ; an extended memory manager IS
        iret                            ; installed - see RMXMS.INC
i2F_xmsaddr:
        push    cs
        pop     es
        mov     bx,OFFSET rx_entry
        iret
i2F_nowin:
        mov     al,0                    ; 386 enhanced mode is not running -
        iret                            ; not yet, which is the point

;--- 1000h: is SHARE loaded?  AL=FFh, yes.  What SHARE means to the
;--- programs that ask is "5Ch works and sharing modes are honoured",
;--- and 5Ch has always succeeded here; the kernel underneath this DOS
;--- has SHARE on by default.  Win32s Setup stops to say "SHARE.EXE is
;--- not loaded" without this (the enhanced-mode sweep, 2026-10-05),
;--- and Office's installers refuse outright.  What is NOT here is the
;--- refusal of a conflicting open or lock - one program, one DOS,
;--- nothing to conflict with it.
;---
;--- NOT WHAT KEPT VSHARE.386 OUT.  Windows for Workgroups 3.11's
;--- VSHARE refused to load on this DOS ("Cannot load a device file
;--- that is specified in SYSTEM.INI ... vshare.386"), and the first
;--- guess was this answer, since on a real DOS a loaded SHARE.EXE
;--- keeps VSHARE out the same way.  It was not: VSHARE has no 1000h in
;--- it anywhere.  Its real-mode start-up reads the byte at offset 4 of
;--- the segment 52h names - DOS 5's DataVersion - and stops unless it
;--- is 0 or 1, and this DOS had its list of lists there.  The block is
;--- laid out as DOS 5's now (RMDOS.INC), and VSHARE loads; once loaded
;--- it answers 1000h itself, in V86, before this sees it.
i2F_share:
        mov     al,0FFh
        iret

;--- 4A10h: SMARTDRV's own interface, answered for the cache in
;--- RMCACHE.INC - a program asking "is SMARTDRV there?" gets BABEh and
;--- the figures, and a program telling it to stop caching a drive is
;--- obeyed.  1606h: Windows is leaving; the cache writes its log if it
;--- was asked to.  Both leave the caller's DS and ES alone.
i2F_bambi:
        jmp     cm_api
i2F_winexit:
IFDEF EXIT_TRACE
        XTMARK16 50,057h                ; 'W'
ENDIF
        call    cm_winexit
        iret
i2F_iret:
        iret

;=====================================================================
; Data
;=====================================================================
rd_dbcs     dw  0

rd_country:
        dw  0                           ; date format, month first
        db  '$',0,0,0,0                 ; currency
        db  ',',0                       ; thousands
        db  '.',0                       ; decimal
        db  '-',0                       ; date separator
        db  ':',0                       ; time separator
        db  0                           ; currency format
        db  2                           ; digits after the point
        db  0                           ; twelve-hour clock
        dd  0                           ; the case-map far call: rd_lowsave
                                        ; points it at the low block's far
                                        ; return (RL_DEVRET).  Zero here was
                                        ; a call into the interrupt table,
                                        ; which WfW 3.11's network made
        db  ',',0                       ; list separator
        db  10 dup(0)
