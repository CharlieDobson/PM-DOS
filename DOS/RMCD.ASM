;=====================================================================
; RMCD.ASM - RMCD.SYS, the CD-ROM while Windows has the machine
;
; THE KERNEL READS A CD ITSELF, IN 32-BIT CODE (ISO9660.INC, UDF.INC,
; ATAPI.INC), AND NONE OF THAT RUNS ONCE IT HAS PARKED.  Under Windows
; the DOS is RMDOS, which has FAT volumes and nothing else, so the CD's
; letter was simply not there - and with the phantom letters RMDOS used
; to report, File Manager showed a D: that listed nothing (Charlie's 486,
; 2026-10-09).
;
; This is that letter's other half, the way MS-DOS had it: a REDIRECTOR,
; as MSCDEX is.  The park loads it beside RMDOS when the kernel has a
; CD-ROM on an IDE channel (PARK.INC, park_loadcd) and tells it the
; channel's ports and which drive; RMDOS marks the letter's current
; directory structure as a redirector's - in use, redirected, and kept
; out of the network's list of connections, as MSCDEX marks it - and
; calls cd_init, which hooks INT 2Fh.  From then on every DOS call that
; names the letter reaches here as INT 2Fh AH=11h with MS-DOS's working
; state in the swappable data area (RMNET.INC says what RMDOS fills in,
; call by call), and the CD-ROM extensions' own calls, AH=15h, are
; answered as MSCDEX 2.23 answers them.  Windows for Workgroups' 32-bit
; file access leaves a drive like this to the DOS underneath, as it
; left MSCDEX's.
;
; WHAT IT READS is the primary ISO 9660 tree of the disc's first
; session, its names as 8.3 - the ";1" gone, the last dot taken as the
; extension, what is too long cut off - which is what Windows 3.1 can
; use.  A Joliet tree's long names, a UDF-only disc and the later
; sessions of a multisession disc are the kernel's business, outside
; Windows.  Everything that would write is refused "access denied", as
; MSCDEX refuses it.
;
; THE DRIVE IS TALKED TO DIRECTLY, by PIO, its interrupt turned off for
; the length of each command and polled - the kernel's ATAPI.INC does
; the same - and turned on again afterwards, for a BIOS disk that may
; share the cable.  Under Windows the ports are the machine's own unless
; LRGDISK.386 holds the cable, and LRGDISK lends it (its ld_vmio): a
; command of ours waits for one of its own, and the other way round.
; WDCTRL, which keeps its cable, puts a CD-ROM on that cable out of
; reach - as it put it out of the reach of every DOS CD-ROM driver.
;
; One 2048-byte block is kept (cd_blk), and a file's data goes straight
; into the caller's buffer when it is whole blocks.
;
; Assembled as a raw image at offset 0, which is the start of the block
; the park loads it into: wasm -q -i=..\INC rmcd.asm, wlink form raw bin
; option offset=0x0 (BUILD.CMD).
;=====================================================================
        .386

        INCLUDE RMDOS.INC
        INCLUDE RMSDA.INC

;--- its header: RC_SIG and the RCH_* offsets are in RMDOS.INC, which
;--- the park and RMDOS read it by

;--- the drive ---------------------------------------------------------
ATA_DATA        EQU     0
ATA_ERR         EQU     1               ; read: error; write: features
ATA_BCLO        EQU     4               ; byte count, low and high: what
ATA_BCHI        EQU     5               ; the drive will send in one burst
ATA_DEV         EQU     6
ATA_STAT        EQU     7               ; read: status; write: command
ST_BSY          EQU     80h
ST_DRQ          EQU     08h
ST_ERR          EQU     01h
CTL_NIEN        EQU     0Ah             ; 08h and no interrupt
CTL_INTS        EQU     08h             ; 08h and the interrupt on
CMD_PACKET      EQU     0A0h
OP_TESTRDY      EQU     00h
OP_SENSE        EQU     03h
OP_READ10       EQU     28h
BLK             EQU     2048
BCL             EQU     0F800h          ; the most we take in one burst:
                                        ; thirty-one blocks, and even
MAXRUN          EQU     30              ; blocks read straight into a
                                        ; caller's buffer in one command,
                                        ; which with the offset under 16
                                        ; RMDOS leaves it stays in its
                                        ; segment
WAITTICKS       EQU     182             ; ten seconds of BIOS ticks: a disc
                                        ; spinning up takes several
SK_NOTREADY     EQU     2
SK_ATTENTION    EQU     6               ; a disc went in or came out
ASC_BECOMING    EQU     0104h           ; sense bytes 12 and 13, as a word:
                                        ; "not ready, becoming ready"
RDY_TRIES       EQU     24              ; half-second waits for a disc that
                                        ; is spinning up: twelve seconds

;--- ISO 9660 ----------------------------------------------------------
PVD_BLK         EQU     16
VD_TYPE         EQU     0
VD_ID           EQU     1               ; "CD001"
PVD_VOLID       EQU     40              ; 32 bytes
PVD_SIZE        EQU     80              ; dd, little-endian half
PVD_ROOT        EQU     156             ; a directory record
PVD_COPYR       EQU     702             ; 37 bytes each
PVD_ABSTR       EQU     739
PVD_BIBLIO      EQU     776
DR_LEN          EQU     0
DR_EXTENT       EQU     2               ; dd
DR_SIZE         EQU     10              ; dd
DR_DATE         EQU     18              ; 7 bytes: years since 1900, month,
                                        ; day, hour, minute, second, zone
DR_FLAGS        EQU     25
DR_NAMELEN      EQU     32
DR_NAME         EQU     33
DRF_HIDDEN      EQU     01h
DRF_DIR         EQU     02h

;--- the twenty-one bytes of a search's state (SEARCHBUF) --------------
SS_DRIVE        EQU     0               ; 80h + the letter, A = 1 - bit 7 is
                                        ; how RMDOS knows a redirector's
SS_PAT          EQU     1               ; 11 bytes, '?' for a wildcard
SS_ATTR         EQU     12
SS_DIR          EQU     13              ; dd  the directory's first block
SS_POS          EQU     17              ; dd  the next record's offset in it;
                                        ; FFFFFFFFh, nothing more
SS_LEN          EQU     21

;--- a file table entry: MS-DOS's own fields, and the redirector's -----
SFT_MODE        EQU     02h
SFT_ATTR        EQU     04h
SFT_INFO        EQU     05h
SFT_DEVPTR      EQU     07h
SFT_CLUS        EQU     0Bh
SFT_TIME        EQU     0Dh
SFT_DATE        EQU     0Fh
SFT_SIZE        EQU     11h
SFT_POS         EQU     15h
SFT_EXTENT      EQU     19h             ; dd  the redirector's from here: the
SFT_GEN         EQU     1Dh             ; dw  file's first block, and which
                                        ;     disc it was on (cd_gen)
SFT_NAME        EQU     20h

;--- a directory entry in a search's answer, after the state -----------
DE_NAME         EQU     0
DE_ATTR         EQU     0Bh
DE_TIME         EQU     16h
DE_DATE         EQU     18h
DE_CLUS         EQU     1Ah
DE_SIZE         EQU     1Ch

ERR_NOTREADY    EQU     21
ERR_READFAULT   EQU     30
ERR_NOMORE      EQU     18
ERR_DISKCHG     EQU     34

_RC     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RC, DS:_RC, ES:NOTHING, SS:NOTHING
        ORG     0

;--- the header --------------------------------------------------------
cd_sig      dd  RC_SIG                  ; also what a file's SFT_DEVPTR
cd_ver      dw  1                       ; points at: one of ours
cd_initp    dw  OFFSET cd_init
cd_letter   db  0
cd_sel      db  0
cd_base     dw  0
cd_ctrl     dw  0
IF ($ - cd_sig) NE RCH_CTRL + 2
        .ERR    <RMCD.ASM: the header and RCH_* disagree>
ENDIF

;=====================================================================
; cd_init - FAR, from RMDOS once the drive's current directory
;           structure is marked.  ES = the block below 64K, where the
;           SDA and the structures are.  INT 2Fh is hooked.  All
;           registers are kept.
;=====================================================================
cd_init:
        push    ax
        push    bx
        push    ds
        push    es
        push    cs
        pop     ds
        mov     [cd_lowseg],es
        xor     ax,ax
        mov     es,ax
        cli
        mov     ax,es:[2Fh * 4]
        mov     word ptr [cd_old2f],ax
        mov     ax,es:[2Fh * 4 + 2]
        mov     word ptr [cd_old2f + 2],ax
        mov     word ptr es:[2Fh * 4],OFFSET cd_i2f
        mov     es:[2Fh * 4 + 2],cs
        sti
        pop     es
        pop     ds
        pop     bx
        pop     ax
        retf

;=====================================================================
; cd_i2f - INT 2Fh.  AH=11h for this letter, and AH=15h; everything
;          else goes on down the chain.
;=====================================================================
cd_i2f:
        cmp     ah,11h
        je      c2f_net
        cmp     ah,15h
        je      c2f_cdx
c2f_chain:
        jmp     dword ptr cs:[cd_old2f]

c2f_net:
        or      al,al
        jz      c2f_chain               ; "is a redirector there?" is not
                                        ; ours to answer
        call    cd_mine                 ; ZF: ours
        jnz     c2f_chain
        sti
        cld
        push    ds
        push    fs                      ; FS too: the name's walk borrows
        push    cs                      ; it, and RMDOS keeps no copy of
        pop     ds                      ; the caller's
        mov     [cd_fn],al
        push    ax
        movzx   ax,al
        mov     [cd_jmp],ax
        pop     ax
        push    bx
        mov     bx,[cd_jmp]
        cmp     bx,21h
        ja      c2f_unsup
        add     bx,bx
        mov     bx,[cd_fntab + bx]
        mov     [cd_jmp],bx
        pop     bx
        jmp     word ptr [cd_jmp]
c2f_unsup:
        pop     bx
        mov     ax,ERR_FUNC
        jmp     cd_fail

; The functions this letter answers, by AL.  Everything that would
; change the disc is "access denied", as MSCDEX has it; a lock on a
; region of a read-only file is not a thing that happens.
cd_fntab    dw  cd_badfn                ; 00
            dw  cd_denied               ; 01 RMDIR
            dw  cd_badfn                ; 02
            dw  cd_denied               ; 03 MKDIR
            dw  cd_badfn                ; 04
            dw  cd_chdir                ; 05
            dw  cd_close                ; 06
            dw  cd_commit               ; 07
            dw  cd_read                 ; 08
            dw  cd_denied               ; 09 WRITE
            dw  cd_badfn                ; 0A lock
            dw  cd_badfn                ; 0B unlock
            dw  cd_diskinfo             ; 0C
            dw  cd_badfn                ; 0D
            dw  cd_denied               ; 0E set attributes
            dw  cd_getattr              ; 0F
            dw  cd_badfn                ; 10
            dw  cd_denied               ; 11 RENAME
            dw  cd_badfn                ; 12
            dw  cd_denied               ; 13 DELETE
            dw  cd_badfn                ; 14
            dw  cd_badfn                ; 15
            dw  cd_open                 ; 16
            dw  cd_denied               ; 17 CREATE
            dw  cd_badfn                ; 18
            dw  cd_badfn                ; 19
            dw  cd_badfn                ; 1A
            dw  cd_findfirst            ; 1B
            dw  cd_findnext             ; 1C
            dw  cd_badfn                ; 1D
            dw  cd_badfn                ; 1E
            dw  cd_badfn                ; 1F
            dw  cd_badfn                ; 20
            dw  cd_seekend              ; 21

;--- the ways out: DS and FS on the stack are the caller's, the carry
;--- and AX the answer.  RETF 2, as MS-DOS's own handler returns, so
;--- the flags are these and not the INT's.
cd_badfn:
        mov     ax,ERR_FUNC
        jmp     short cd_fail
cd_denied:
        mov     ax,ERR_ACCESS
cd_fail:
        pop     fs
        pop     ds
        stc
        retf    2
cd_ok:
        pop     fs
        pop     ds
        clc
        retf    2

;---------------------------------------------------------------------
; cd_mine - AL = the 11xxh function.  ZF if the call is for this letter.
;           Registers kept.
;
; A call about a NAME OR A DRIVE carries the drive's structure in the
; SDA's THISCDS, which RMDOS sets as MS-DOS's TransPath does; a call on
; a HANDLE has its file table entry in ES:DI, whose device pointer is
; this image's signature for a file opened here; a find-next has the
; search's twenty-one bytes in ES:DI, the first of them 80h + the
; letter.  Process ends and the redirection list are never ours.
;---------------------------------------------------------------------
cd_mine:
        push    ax
        push    bx
        push    si
        push    ds
        cmp     al,06h
        je      cdm_sft
        cmp     al,07h
        je      cdm_sft
        cmp     al,08h
        je      cdm_sft
        cmp     al,09h
        je      cdm_sft
        cmp     al,0Ah
        je      cdm_sft
        cmp     al,0Bh
        je      cdm_sft
        cmp     al,21h
        je      cdm_sft
        cmp     al,1Ch
        je      cdm_next
        cmp     al,1Dh
        je      cdm_no
        cmp     al,1Eh
        je      cdm_no
        cmp     al,1Fh
        je      cdm_no
        cmp     al,22h
        jae     cdm_no
        ; by THISCDS
        mov     ds,word ptr cs:[cd_lowseg]
        mov     ax,ds
        cmp     word ptr ds:[RL_SDA + SD_THISCDS + 2],ax
        jne     cdm_out
        movzx   bx,byte ptr cs:[cd_letter]
        imul    bx,bx,RL_CDSLEN
        add     bx,RL_CDS
        cmp     word ptr ds:[RL_SDA + SD_THISCDS],bx
        jmp     short cdm_out
cdm_sft:
        cmp     word ptr es:[di+SFT_DEVPTR],OFFSET cd_sig
        jne     cdm_out
        mov     ax,cs
        cmp     word ptr es:[di+SFT_DEVPTR+2],ax
        jmp     short cdm_out
cdm_next:
        mov     al,byte ptr cs:[cd_letter]
        add     al,81h
        cmp     byte ptr es:[di+SS_DRIVE],al
        jmp     short cdm_out
cdm_no:
        or      sp,sp                   ; NZ
cdm_out:
        pop     ds
        pop     si
        pop     bx
        pop     ax
        ret

;=====================================================================
; The 11xxh functions.  Entered with DS = this segment and the caller's
; DS and FS on the stack; the caller's other registers as they came.
;=====================================================================

;--- 05h CHDIR: is the SDA's name a directory? -------------------------
cd_chdir:
        call    cd_ready
        jc      cd_fail
        call    cd_resolve              ; -> the record, or the root
        jc      cd_fail
        test    byte ptr [cd_rflags],DRF_DIR
        jz      cdc_nopath
        jmp     cd_ok
cdc_nopath:
        mov     ax,ERR_NOPATH
        jmp     cd_fail

;--- 06h CLOSE: ES:DI the entry.  DOS takes the reference count down
;--- and frees it; the redirector has nothing of its own to let go of.
;--- 07h COMMIT likewise.
cd_close:
        dec     word ptr es:[di]        ; sf_ref_count, as a redirector's
        jns     cd_commit               ; close takes it down (DOS6SRC
        mov     word ptr es:[di],0      ; dos/close.asm, Net_Close)
cd_commit:
        jmp     cd_ok

;--- 0Ch DISK INFO: AL = sectors a cluster, AH = media, BX = clusters,
;--- CX = bytes a sector, DX = free.  A cluster is as many blocks as it
;--- takes to bring the count under 64K, since BX is a word; nothing is
;--- free.
cd_diskinfo:
        call    cd_ready
        jc      cd_fail
        mov     eax,[cd_volblks]
        mov     cl,1
cdi_scale:
        cmp     eax,0FFFFh
        jbe     cdi_fits
        shr     eax,1
        shl     cl,1
        jmp     short cdi_scale
cdi_fits:
        mov     bx,ax
        mov     al,cl
        mov     ah,0F8h
        mov     cx,BLK
        xor     dx,dx
        jmp     cd_ok

;--- 0Fh GET ATTRIBUTES: AX = the attributes, BX:DI = the size, CX =
;--- the time, DX = the date.
cd_getattr:
        call    cd_ready
        jc      cd_fail
        call    cd_resolve
        jc      cd_fail
        call    cd_attr                 ; -> AL
        movzx   ax,al
        mov     di,word ptr [cd_rsize]
        mov     bx,word ptr [cd_rsize + 2]
        test    al,ATTR_DIR             ; a directory: size 0, as in a
        jz      cga_file                ; search
        xor     di,di
        xor     bx,bx
cga_file:
        mov     cx,[cd_rtime]
        mov     dx,[cd_rdate]
        jmp     cd_ok

;--- 16h OPEN: ES:DI the entry, its mode already in it.  Reading only;
;--- a directory is not a file.
cd_open:
        test    byte ptr es:[di+SFT_MODE],03h
        jnz     cd_denied
        mov     [cd_sftoff],di          ; THE ENTRY IS KEPT: the walk
        mov     [cd_sftseg],es          ; below uses DI and ES, and an
        call    cd_ready                ; entry written through what they
        jc      cd_fail                 ; were left at went into RMDOS's
        call    cd_resolve              ; own segment (2026-10-09)
        jc      cd_fail
        test    byte ptr [cd_rflags],DRF_DIR
        jnz     cd_denied
        call    cd_attr
        les     di,dword ptr [cd_sftoff]
        mov     es:[di+SFT_ATTR],al
        mov     al,[cd_letter]
        and     ax,003Fh
        or      ax,8040h                ; a redirector's file, not written
        mov     es:[di+SFT_INFO],ax
        mov     word ptr es:[di+SFT_DEVPTR],OFFSET cd_sig
        mov     es:[di+SFT_DEVPTR+2],cs
        mov     eax,[cd_rext]
        mov     es:[di+SFT_CLUS],ax
        mov     es:[di+SFT_EXTENT],eax
        mov     ax,[cd_gen]
        mov     es:[di+SFT_GEN],ax
        mov     ax,[cd_rtime]
        mov     es:[di+SFT_TIME],ax
        mov     ax,[cd_rdate]
        mov     es:[di+SFT_DATE],ax
        mov     eax,[cd_rsize]
        mov     es:[di+SFT_SIZE],eax
        mov     dword ptr es:[di+SFT_POS],0
        push    si
        push    di
        mov     si,OFFSET cd_fcb
        add     di,SFT_NAME
        mov     cx,11
        rep     movsb
        pop     di
        pop     si
        jmp     cd_ok

;--- 21h SEEK FROM THE END: ES:DI the entry, CX:DX the distance.  DX:AX
;--- = where that is, and the entry is left there.
cd_seekend:
        push    dx
        mov     ax,cx
        shl     eax,16
        pop     ax                      ; EAX = CX:DX
        add     eax,es:[di+SFT_SIZE]
        mov     es:[di+SFT_POS],eax
        mov     edx,eax
        shr     edx,16
        jmp     cd_ok

;--- 08h READ: ES:DI the entry, CX bytes, into the SDA's DTA, from the
;--- entry's position.  CX = what came; the position moves on.
cd_read:
        mov     ax,es:[di+SFT_GEN]
        cmp     ax,[cd_gen]
        jne     crd_changed
        call    cd_ready
        jc      cd_fail
        mov     ax,es:[di+SFT_GEN]      ; cd_ready may have found it
        cmp     ax,[cd_gen]             ; changed just now
        jne     crd_changed
        mov     [cd_sftoff],di
        mov     [cd_sftseg],es
        mov     eax,es:[di+SFT_SIZE]
        sub     eax,es:[di+SFT_POS]
        jbe     crd_none                ; at or past the end
        movzx   ecx,cx
        cmp     ecx,eax
        jbe     crd_count
        mov     ecx,eax
crd_count:
        mov     [cd_want],ecx
        mov     dword ptr [cd_done],0
        push    fs
        mov     fs,[cd_lowseg]
        mov     ax,word ptr fs:[RL_SDA + SD_DTA]
        mov     [cd_dst],ax
        mov     ax,word ptr fs:[RL_SDA + SD_DTA + 2]
        mov     [cd_dstseg],ax
        pop     fs
crd_more:
        mov     ecx,[cd_want]
        sub     ecx,[cd_done]
        jz      crd_end
        les     di,dword ptr [cd_sftoff]
        mov     eax,es:[di+SFT_POS]
        mov     ebx,eax
        shr     eax,11                  ; the block it is in
        add     eax,es:[di+SFT_EXTENT]
        and     ebx,BLK - 1             ; and where in it
        jnz     crd_part
        cmp     ecx,BLK
        jb      crd_part
        ; WHOLE BLOCKS, straight into the caller's buffer
        shr     ecx,11
        cmp     ecx,MAXRUN
        jbe     crd_run
        mov     ecx,MAXRUN
crd_run:
        les     di,dword ptr [cd_dst]
        push    ecx
        call    cd_readblks             ; EAX = the first, CX = how many
        pop     ecx
        jc      crd_err
        shl     ecx,11
        jmp     short crd_moved
crd_part:
        ; part of one block, through cd_blk
        call    cd_getblk               ; EAX -> cd_blk
        jc      crd_err
        mov     edx,BLK
        sub     edx,ebx                 ; what is left of the block
        cmp     ecx,edx
        jbe     crd_partn
        mov     ecx,edx
crd_partn:
        push    ecx
        les     di,dword ptr [cd_dst]
        lea     si,[cd_blk + bx]
        rep     movsb
        pop     ecx
crd_moved:
        add     [cd_dst],cx             ; RMDOS keeps it within its segment
        add     [cd_done],ecx
        les     di,dword ptr [cd_sftoff]
        add     es:[di+SFT_POS],ecx
        jmp     crd_more
crd_end:
        les     di,dword ptr [cd_sftoff]
        mov     cx,word ptr [cd_done]
        jmp     cd_ok
crd_none:
        xor     cx,cx
        jmp     cd_ok
crd_err:
        mov     ecx,[cd_done]           ; what came before the failure
        or      ecx,ecx                 ; is an answer; nothing is an error
        jnz     crd_end
        mov     ax,ERR_READFAULT        ; (AX is the block: the readers
        jmp     cd_fail                 ; keep their registers)
crd_changed:
        mov     ax,ERR_DISKCHG
        jmp     cd_fail

;--- 1Bh FIND FIRST: the SDA's name and search attribute, the answer
;--- into SEARCHBUF (the SDA's DTA points there, as DOS points it).
cd_findfirst:
        call    cd_ready
        jc      cd_fail
        push    es
        mov     es,[cd_lowseg]
        mov     al,byte ptr es:[RL_SDA + SD_SATTR]
        mov     [cd_sattr],al
        pop     es
        call    cd_parent               ; -> cd_dirext, cd_dirsize, the
        jc      cff_fail                ; last name in cd_fcb
        ; the search's state, in SEARCHBUF
        push    es
        mov     es,[cd_lowseg]
        mov     di,RL_SDA + SD_SRCH
        mov     al,[cd_letter]
        add     al,81h
        mov     es:[di+SS_DRIVE],al
        push    di
        add     di,SS_PAT
        mov     si,OFFSET cd_fcb
        mov     cx,11
        rep     movsb
        pop     di
        mov     al,[cd_sattr]
        mov     es:[di+SS_ATTR],al
        mov     eax,[cd_dirext]
        mov     es:[di+SS_DIR],eax
        mov     dword ptr es:[di+SS_POS],0
        ; THE VOLUME'S NAME, for a search that asks for it: the label is
        ; in the root and nowhere else, and it is the only answer
        test    byte ptr [cd_sattr],ATTR_VOLUME
        jz      cff_files
        mov     dword ptr es:[di+SS_POS],0FFFFFFFFh
        call    cd_volent
        pop     es
        jc      cff_nofile
        jmp     cd_ok
cff_files:
        pop     es
        call    cd_scan
        jc      cff_nofile
        jmp     cd_ok
cff_nofile:
        mov     ax,ERR_NOFILE
cff_fail:
        jmp     cd_fail

;--- 1Ch FIND NEXT: ES:DI = SEARCHBUF, with the state DOS gave back.
cd_findnext:
        call    cd_ready
        jc      cd_fail
        mov     ax,ERR_NOMORE
        cmp     dword ptr es:[di+SS_POS],0FFFFFFFFh
        je      cfn_fail
        mov     eax,es:[di+SS_DIR]
        mov     [cd_dirext],eax
        call    cd_dirlen               ; -> cd_dirsize
        jc      cfn_fail
        push    si
        push    di
        mov     si,di
        add     si,SS_PAT
        mov     di,OFFSET cd_fcb
        push    ds
        push    es
        push    ds
        push    es
        pop     ds
        pop     es
        mov     cx,11
        rep     movsb
        pop     es
        pop     ds
        pop     di
        pop     si
        mov     al,es:[di+SS_ATTR]
        mov     [cd_sattr],al
        call    cd_scan
        jc      cfn_none
        jmp     cd_ok
cfn_none:
        mov     ax,ERR_NOMORE
cfn_fail:
        jmp     cd_fail

;=====================================================================
; cd_scan - the next record of the directory cd_dirext (cd_dirsize
;           bytes long) at or past SEARCHBUF's SS_POS that matches
;           cd_fcb and cd_sattr, into SEARCHBUF: the entry after the
;           state, and SS_POS past it.  CF if there are no more (SS_POS
;           is left at the end).  Registers but DS are lost.
;
; DOS's rules for what a search sees: a normal file always, a hidden
; one or a directory only if the attribute asks for it.  The root has
; no "." or "..", as on a FAT disk; every other directory has both.
;=====================================================================
cd_scan:
        push    es
        mov     es,[cd_lowseg]
        mov     eax,dword ptr es:[RL_SDA + SD_SRCH + SS_POS]
        pop     es
        mov     [cd_pos],eax
csc_next:
        mov     eax,[cd_pos]
        cmp     eax,[cd_dirsize]
        jae     csc_end
        call    cd_recat                ; -> SI = the record, or ZF: none
        jz      csc_skipblk             ; here (the rest of the block)
        movzx   eax,byte ptr [si+DR_LEN]
        add     [cd_pos],eax
        call    cd_takerec              ; the record into cd_r*, cd_name
        call    cd_isdot                ; AL = 1 ".", 2 "..", 0 neither
        or      al,al
        jz      csc_named
        mov     eax,[cd_dirext]
        cmp     eax,[cd_rootext]
        je      csc_next                ; not in the root
csc_named:
        mov     si,OFFSET cd_name
        mov     di,OFFSET cd_ename
        call    cd_tofcb
        mov     si,OFFSET cd_fcb
        mov     di,OFFSET cd_ename
        call    cd_match
        jne     csc_next
        call    cd_attr                 ; -> AL
        mov     ah,al
        and     ah,ATTR_HIDDEN OR ATTR_DIR
        not     ah
        or      ah,[cd_sattr]           ; every one of its special bits
        not     ah                      ; must have been asked for
        and     ah,ATTR_HIDDEN OR ATTR_DIR
        jnz     csc_next
        ; the answer
        push    es
        mov     es,[cd_lowseg]
        mov     di,RL_SDA + SD_SRCH + SS_LEN
        mov     si,OFFSET cd_ename
        mov     cx,11
        rep     movsb                   ; DE_NAME
        stosb                           ; DE_ATTR
        push    ax
        xor     al,al
        mov     cx,DE_TIME - DE_ATTR - 1
        rep     stosb
        pop     ax
        mov     di,RL_SDA + SD_SRCH + SS_LEN
        mov     ax,[cd_rtime]
        mov     es:[di+DE_TIME],ax
        mov     ax,[cd_rdate]
        mov     es:[di+DE_DATE],ax
        mov     ax,word ptr [cd_rext]
        mov     es:[di+DE_CLUS],ax
        mov     eax,[cd_rsize]
        test    byte ptr [cd_rflags],DRF_DIR
        jz      csc_size
        xor     eax,eax                 ; a directory is size 0 to DOS, as
csc_size:                               ; on a FAT disk and under MSCDEX
        mov     es:[di+DE_SIZE],eax
        mov     eax,[cd_pos]
        mov     dword ptr es:[RL_SDA + SD_SRCH + SS_POS],eax
        pop     es
        clc
        ret
csc_skipblk:
        mov     eax,[cd_pos]
        add     eax,BLK
        and     eax,NOT (BLK - 1)
        mov     [cd_pos],eax
        jmp     csc_next
csc_end:
        push    es
        mov     es,[cd_lowseg]
        mov     dword ptr es:[RL_SDA + SD_SRCH + SS_POS],0FFFFFFFFh
        pop     es
        stc
        ret

;--- cd_volent - the volume's name as a search's answer: the eleven
;--- characters of the descriptor's volume identifier, attribute 08h.
;--- Only in the root, and only if the pattern takes it.  CF if not.
cd_volent:
        mov     eax,[cd_dirext]
        cmp     eax,[cd_rootext]
        jne     cve_none
        mov     si,OFFSET cd_label
        mov     di,OFFSET cd_ename
        push    es
        push    ds
        pop     es
        mov     cx,11
        rep     movsb
        pop     es
        mov     si,OFFSET cd_fcb
        mov     di,OFFSET cd_ename
        call    cd_match
        jne     cve_none
        push    es
        mov     es,[cd_lowseg]
        mov     di,RL_SDA + SD_SRCH + SS_LEN
        mov     si,OFFSET cd_ename
        mov     cx,11
        rep     movsb
        mov     al,ATTR_VOLUME
        stosb
        xor     al,al
        mov     cx,32 - 12
        rep     stosb
        pop     es
        clc
        ret
cve_none:
        stc
        ret

;=====================================================================
; cd_recat - the record at offset cd_pos of the directory cd_dirext.
;            -> SI = it, in cd_blk; ZF if there is none at that offset
;            (a zero length: the rest of the block is padding).  CF
;            with AX on a read failure is not separated from ZF here -
;            a block that cannot be read ends the directory.
;=====================================================================
cd_recat:
        mov     eax,[cd_pos]
        shr     eax,11
        add     eax,[cd_dirext]
        call    cd_getblk
        jc      crc_none
        mov     si,word ptr [cd_pos]
        and     si,BLK - 1
        add     si,OFFSET cd_blk
        cmp     byte ptr [si+DR_LEN],DR_NAME
        jb      crc_none                ; 0, or too short to be a record
        or      sp,sp                   ; NZ
        ret
crc_none:
        cmp     al,al                   ; ZF
        ret

;=====================================================================
; cd_takerec - SI = a directory record.  Its block, size, flags, DOS
;              time and date into cd_r*; its identifier, ";1" and all,
;              into cd_name as text.  Registers but SI lost.
;=====================================================================
cd_takerec:
        mov     eax,[si+DR_EXTENT]
        mov     [cd_rext],eax
        mov     eax,[si+DR_SIZE]
        mov     [cd_rsize],eax
        mov     al,[si+DR_FLAGS]
        mov     [cd_rflags],al
        ; the date: years since 1900, and DOS counts from 1980
        movzx   ax,byte ptr [si+DR_DATE]
        sub     ax,80
        jae     ctr_year
        mov     word ptr [cd_rdate],(1 SHL 5) OR 1      ; before 1980: 1-1-80
        mov     word ptr [cd_rtime],0
        jmp     short ctr_name
ctr_year:
        shl     ax,9
        movzx   bx,byte ptr [si+DR_DATE+1]
        shl     bx,5
        or      ax,bx
        movzx   bx,byte ptr [si+DR_DATE+2]
        or      ax,bx
        mov     [cd_rdate],ax
        movzx   ax,byte ptr [si+DR_DATE+3]
        shl     ax,11
        movzx   bx,byte ptr [si+DR_DATE+4]
        shl     bx,5
        or      ax,bx
        movzx   bx,byte ptr [si+DR_DATE+5]
        shr     bx,1
        or      ax,bx
        mov     [cd_rtime],ax
ctr_name:
        push    si
        push    es
        push    ds
        pop     es
        movzx   cx,byte ptr [si+DR_NAMELEN]
        cmp     cx,CD_NAMEMAX
        jbe     ctr_len
        mov     cx,CD_NAMEMAX
ctr_len:
        add     si,DR_NAME
        mov     di,OFFSET cd_name
        rep     movsb
        mov     byte ptr [di],0
        pop     es
        pop     si
        ret

;--- cd_isdot - AL = 1 for the record of the directory itself (its
;--- identifier one byte, 00h), 2 for its parent (01h), 0 for a name.
cd_isdot:
        xor     al,al
        cmp     byte ptr [cd_name+1],0
        jne     cid_out
        mov     ah,[cd_name]
        cmp     ah,1
        ja      cid_out
        mov     al,ah
        inc     al
cid_out:
        ret

;=====================================================================
; cd_tofcb - SI = an identifier as text (cd_name: ISO's, with ";1",
;            or "." and ".." as bytes 00h and 01h), DI = eleven bytes.
;            Its 8.3 spelling, upper case, blank padded: the version
;            gone, the LAST dot the extension (the kernel's rule, in
;            ISO9660.INC's iso_mk83), anything too long cut off, and a
;            character DOS will not have in a name made '_'.
;=====================================================================
cd_tofcb:
        push    es
        push    ds
        pop     es
        push    di
        mov     al,' '
        mov     cx,11
        rep     stosb
        pop     di
        ; . and ..
        cmp     byte ptr [si+1],0
        jne     ctf_text
        cmp     byte ptr [si],1
        ja      ctf_text
        mov     byte ptr [di],'.'
        cmp     byte ptr [si],0
        je      ctf_done
        mov     byte ptr [di+1],'.'
        jmp     short ctf_done
ctf_text:
        ; the length up to ';', and the last dot before it
        xor     bx,bx                   ; BX = length
        mov     dx,0FFFFh               ; DX = the last dot, none
ctf_len:
        mov     al,[si+bx]
        or      al,al
        jz      ctf_measured
        cmp     al,';'
        je      ctf_measured
        cmp     al,'.'
        jne     ctf_lnext
        mov     dx,bx
ctf_lnext:
        inc     bx
        jmp     short ctf_len
ctf_measured:
        ; the base: up to the last dot, or the whole if there is none
        mov     cx,bx
        cmp     dx,0FFFFh
        je      ctf_base
        mov     cx,dx
ctf_base:
        push    si
        push    di
        cmp     cx,8
        jbe     ctf_b8
        mov     cx,8
ctf_b8:
        jcxz    ctf_bdone
ctf_bloop:
        lodsb
        call    cd_dosch
        stosb
        loop    ctf_bloop
ctf_bdone:
        pop     di
        pop     si
        cmp     dx,0FFFFh
        je      ctf_done
        ; the extension: after the dot, up to three
        mov     cx,bx
        sub     cx,dx
        dec     cx
        cmp     cx,3
        jbe     ctf_e3
        mov     cx,3
ctf_e3:
        jcxz    ctf_done
        push    si
        add     si,dx
        inc     si
        add     di,8
ctf_eloop:
        lodsb
        call    cd_dosch
        stosb
        loop    ctf_eloop
        pop     si
ctf_done:
        pop     es
        ret

;--- cd_dosch - AL as a character an 8.3 name may hold: upper case, and
;--- '_' for a blank, a dot or anything DOS keeps out of names.
cd_dosch:
        cmp     al,'a'
        jb      cdc_notlow
        cmp     al,'z'
        ja      cdc_notlow
        sub     al,20h
        ret
cdc_notlow:
        cmp     al,'0'
        jb      cdc_punct
        cmp     al,'9'
        jbe     cdc_ok
        cmp     al,'A'
        jb      cdc_punct
        cmp     al,'Z'
        jbe     cdc_ok
cdc_punct:
        push    di
        push    cx
        push    es
        push    ds
        pop     es
        mov     di,OFFSET cd_okch
        mov     cx,CD_OKCHLEN
        repne   scasb
        pop     es
        pop     cx
        pop     di
        je      cdc_ok
        mov     al,'_'
cdc_ok:
        ret

;--- cd_match - SI = a pattern, DI = a name, eleven bytes each.  ZF if
;--- they match: every character the same, or '?' in the pattern.
cd_match:
        push    cx
        push    si
        push    di
        mov     cx,11
cma_one:
        mov     al,[si]
        cmp     al,'?'
        je      cma_next
        cmp     al,[di]
        jne     cma_out
cma_next:
        inc     si
        inc     di
        loop    cma_one
        cmp     al,al
cma_out:
        pop     di
        pop     si
        pop     cx
        ret

;--- cd_attr - AL = the DOS attributes of the record in cd_r*: read-only
;--- always, a directory, hidden when the disc says so.
cd_attr:
        mov     al,ATTR_READONLY
        test    byte ptr [cd_rflags],DRF_DIR
        jz      cat_file
        mov     al,ATTR_DIR OR ATTR_READONLY
cat_file:
        test    byte ptr [cd_rflags],DRF_HIDDEN
        jz      cat_out
        or      al,ATTR_HIDDEN
cat_out:
        ret

;=====================================================================
; cd_resolve - the SDA's name (WFP_START: "D:\DIR\NAME.EXT"), walked
;              from the root.  -> the record in cd_r*, its last
;              component in cd_fcb.  The root itself answers as a
;              directory.  CF with AX = 2 or 3 if it is not there.
; cd_parent  - the same, but the last component is NOT looked up: the
;              directory it would be in -> cd_dirext and cd_dirsize, and
;              the component (a pattern, perhaps) in cd_fcb.
;=====================================================================
cd_resolve:
        mov     byte ptr [cd_wantlast],1
        jmp     short crs_go
cd_parent:
        mov     byte ptr [cd_wantlast],0
crs_go:
        push    es
        mov     es,[cd_lowseg]
        mov     si,word ptr es:[RL_SDA + SD_WFP]
        pop     es
        add     si,3                    ; past "D:\"
        mov     [cd_namep],si
        mov     eax,[cd_rootext]
        mov     [cd_dirext],eax
        mov     eax,[cd_rootsize]
        mov     [cd_dirsize],eax
        ; the root's own record, for a name that is the root
        mov     eax,[cd_rootext]
        mov     [cd_rext],eax
        mov     eax,[cd_rootsize]
        mov     [cd_rsize],eax
        mov     byte ptr [cd_rflags],DRF_DIR
        mov     word ptr [cd_rdate],(1 SHL 5) OR 1
        mov     word ptr [cd_rtime],0
        mov     di,OFFSET cd_fcb        ; an empty last name, for the root
        mov     al,' '
        push    es
        push    ds
        pop     es
        mov     cx,11
        rep     stosb
        pop     es
crs_comp:
        call    cd_component            ; -> cd_fcb, ZF if it was the last
        jc      crs_done                ; nothing left: the root
        jnz     crs_middle
        ; the last component
        cmp     byte ptr [cd_wantlast],0
        je      crs_done
        call    cd_lookup
        jc      crs_nofile
        jmp     short crs_done
crs_middle:
        call    cd_lookup               ; a directory on the way
        jc      crs_nopath
        test    byte ptr [cd_rflags],DRF_DIR
        jz      crs_nopath
        mov     eax,[cd_rext]
        mov     [cd_dirext],eax
        mov     eax,[cd_rsize]
        mov     [cd_dirsize],eax
        jmp     short crs_comp
crs_done:
        clc
        ret
crs_nofile:
        mov     ax,ERR_NOFILE
        stc
        ret
crs_nopath:
        mov     ax,ERR_NOPATH
        stc
        ret

;--- cd_component - the next component of the name at [cd_namep], into
;--- cd_fcb as eleven bytes, and [cd_namep] past it and its backslash.
;--- CF if there is no component at all; otherwise ZF if it is the last.
cd_component:
        push    es
        mov     es,[cd_lowseg]
        mov     si,[cd_namep]
        cmp     byte ptr es:[si],0
        je      ccm_none
        push    di
        mov     di,OFFSET cd_fcb
        push    ds
        pop     fs                      ; FS:DI = the field, ES:SI the name
        mov     cx,11
ccm_blank:
        mov     byte ptr fs:[di],' '
        inc     di
        loop    ccm_blank
        mov     di,OFFSET cd_fcb
        mov     cx,8
ccm_base:
        mov     al,es:[si]
        or      al,al
        jz      ccm_end
        cmp     al,'\'
        je      ccm_sep
        cmp     al,'.'
        je      ccm_dot
        inc     si
        jcxz    ccm_base                ; past eight: dropped
        mov     fs:[di],al
        inc     di
        dec     cx
        jmp     short ccm_base
ccm_dot:
        ; "." and ".." never reach here: RMDOS resolved them
        inc     si
        mov     di,OFFSET cd_fcb + 8
        mov     cx,3
ccm_ext:
        mov     al,es:[si]
        or      al,al
        jz      ccm_end
        cmp     al,'\'
        je      ccm_sep
        inc     si
        jcxz    ccm_ext
        mov     fs:[di],al
        inc     di
        dec     cx
        jmp     short ccm_ext
ccm_sep:
        inc     si
        mov     [cd_namep],si
        pop     di
        pop     es
        or      sp,sp                   ; NZ, NC: more to come
        ret
ccm_end:
        mov     [cd_namep],si
        pop     di
        pop     es
        cmp     al,al                   ; ZF, NC: the last
        ret
ccm_none:
        pop     es
        stc
        ret

;--- cd_lookup - cd_fcb in the directory cd_dirext.  -> its record in
;--- cd_r*.  CF if there is none.
cd_lookup:
        mov     dword ptr [cd_pos],0
clk_next:
        mov     eax,[cd_pos]
        cmp     eax,[cd_dirsize]
        jae     clk_none
        call    cd_recat
        jz      clk_skipblk
        movzx   eax,byte ptr [si+DR_LEN]
        add     [cd_pos],eax
        call    cd_takerec
        call    cd_isdot
        or      al,al
        jnz     clk_next
        mov     si,OFFSET cd_name
        mov     di,OFFSET cd_ename
        call    cd_tofcb
        mov     si,OFFSET cd_ename
        mov     di,OFFSET cd_fcb
        mov     cx,11
        push    es
        push    ds
        pop     es
        repe    cmpsb
        pop     es
        jne     clk_next
        clc
        ret
clk_skipblk:
        mov     eax,[cd_pos]
        add     eax,BLK
        and     eax,NOT (BLK - 1)
        mov     [cd_pos],eax
        jmp     short clk_next
clk_none:
        stc
        ret

;--- cd_dirlen - cd_dirext's length, out of its own "." record: the
;--- first one in its first block.  -> cd_dirsize.  CF if unreadable.
cd_dirlen:
        mov     eax,[cd_dirext]
        cmp     eax,[cd_rootext]
        jne     cdl_read
        mov     eax,[cd_rootsize]
        mov     [cd_dirsize],eax
        clc
        ret
cdl_read:
        call    cd_getblk
        jc      cdl_fail
        mov     eax,dword ptr [cd_blk + DR_SIZE]
        mov     [cd_dirsize],eax
        clc
        ret
cdl_fail:
        stc
        ret

;=====================================================================
; cd_ready - is there a disc, and is it the one we know?  -> CF with AX
;            = 21 (not ready) if not, or AX = 30 if it cannot be read.
;            Mounts it - the primary volume descriptor - when it is new.
;            Asks the drive at most once a second.  Registers but AX
;            and the flags are kept.
;=====================================================================
cd_ready:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    es
        call    cd_ticks                ; EAX
        mov     edx,eax
        sub     edx,[cd_asked]
        cmp     byte ptr [cd_mounted],0
        je      crd_ask
        cmp     edx,18
        jb      crd_have                ; asked within the second: as it was
crd_ask:
        mov     [cd_asked],eax
        mov     byte ptr [cd_tries],RDY_TRIES
crd_again:
        call    cd_testready
        jnc     crd_isready
        ; why: a disc changed is asked again at once; one spinning up
        ; ("not ready, becoming ready" - a disc just put in) is waited
        ; for, half a second at a time, as long as a disc takes; a drive
        ; with nothing in it is not ready, and says so
        cmp     byte ptr [cd_skey],SK_ATTENTION
        jne     crd_spin
        mov     byte ptr [cd_mounted],0
        jmp     short crd_retry
crd_spin:
        cmp     byte ptr [cd_skey],SK_NOTREADY
        jne     crd_notready
        cmp     word ptr [cd_sense+12],ASC_BECOMING
        jne     crd_notready
        call    cd_ticks
        mov     edx,eax
crd_wait:
        call    cd_ticks
        sub     eax,edx
        cmp     eax,9
        jb      crd_wait
crd_retry:
        dec     byte ptr [cd_tries]
        jnz     crd_again
        jmp     short crd_notready
crd_isready:
        cmp     byte ptr [cd_mounted],0
        jne     crd_have
        call    cd_mount
        jc      crd_bad
crd_have:
        clc
        jmp     short crd_out
crd_notready:
        mov     byte ptr [cd_mounted],0
        mov     ax,ERR_NOTREADY
        stc
        jmp     short crd_out
crd_bad:
        mov     ax,ERR_READFAULT
        stc
crd_out:
        pop     es
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- cd_mount - the primary volume descriptor: the root, the size, the
;--- name.  A new disc is a new generation, and a file opened on the
;--- last one will not read this one.  CF if there is no ISO 9660 here.
cd_mount:
        mov     dword ptr [cd_blklba],0FFFFFFFFh        ; the block kept is
        mov     eax,PVD_BLK                             ; the last disc's
cmt_desc:
        push    eax
        call    cd_getblk
        pop     eax
        jc      cmt_fail
        cmp     dword ptr [cd_blk + VD_ID],'00DC'       ; "CD00"
        jne     cmt_fail
        cmp     byte ptr [cd_blk + VD_ID + 4],'1'
        jne     cmt_fail
        cmp     byte ptr [cd_blk + VD_TYPE],1
        je      cmt_pvd
        cmp     byte ptr [cd_blk + VD_TYPE],0FFh
        je      cmt_fail                ; the end, and no primary
        inc     eax
        cmp     eax,PVD_BLK + 32
        jb      cmt_desc
        jmp     short cmt_fail
cmt_pvd:
        mov     [cd_pvdblk],eax
        mov     eax,dword ptr [cd_blk + PVD_SIZE]
        mov     [cd_volblks],eax
        mov     eax,dword ptr [cd_blk + PVD_ROOT + DR_EXTENT]
        mov     [cd_rootext],eax
        mov     eax,dword ptr [cd_blk + PVD_ROOT + DR_SIZE]
        mov     [cd_rootsize],eax
        ; the label: the identifier's first eleven characters, as a name
        push    es
        push    ds
        pop     es
        mov     si,OFFSET cd_blk + PVD_VOLID
        mov     di,OFFSET cd_label
        mov     cx,11
cmt_lab:
        lodsb
        cmp     al,' '
        je      cmt_labch
        call    cd_dosch
cmt_labch:
        stosb
        loop    cmt_lab
        pop     es
        inc     word ptr [cd_gen]
        mov     byte ptr [cd_mounted],1
        clc
        ret
cmt_fail:
        stc
        ret

;--- cd_ticks - EAX = the BIOS's tick count
cd_ticks:
        push    es
        xor     ax,ax
        mov     es,ax
        mov     eax,es:[046Ch]
        pop     es
        ret

;=====================================================================
; cd_getblk - EAX = a block.  It, in cd_blk (kept: asked again, it is
;             not read again).  CF on a failure.  Registers kept but the
;             flags.
; cd_readblks - EAX = the first block, CX = how many, ES:DI = where.
;             CF on a failure.  Registers kept but the flags.
;=====================================================================
cd_getblk:
        cmp     eax,[cd_blklba]
        je      cgb_have
        push    es
        push    di
        push    cx
        push    ds
        pop     es
        mov     di,OFFSET cd_blk
        mov     cx,1
        mov     dword ptr [cd_blklba],0FFFFFFFFh
        call    cd_readblks
        pop     cx
        pop     di
        pop     es
        jc      cgb_out
        mov     [cd_blklba],eax
cgb_have:
        clc
cgb_out:
        ret

cd_readblks:
        push    eax
        push    ebx
        push    cx
        call    cd_pktzero
        mov     byte ptr [cd_pkt+0],OP_READ10
        mov     ebx,eax                 ; the block, big-endian
        rol     ebx,8
        mov     byte ptr [cd_pkt+2],bl
        rol     ebx,8
        mov     byte ptr [cd_pkt+3],bl
        rol     ebx,8
        mov     byte ptr [cd_pkt+4],bl
        rol     ebx,8
        mov     byte ptr [cd_pkt+5],bl
        mov     byte ptr [cd_pkt+7],ch
        mov     byte ptr [cd_pkt+8],cl
        movzx   ecx,cx
        shl     ecx,11
        call    cd_command              ; -> EAX bytes
        jc      crb_out
        cmp     eax,ecx
        jae     crb_out                 ; NC
        stc                             ; short: a failure
crb_out:
        pop     cx
        pop     ebx
        pop     eax
        ret

;--- cd_testready - TEST UNIT READY, and REQUEST SENSE if not: cd_skey
;--- says why.  CF if not ready.
cd_testready:
        push    eax
        push    ecx
        push    di
        push    es
        call    cd_pktzero              ; OP_TESTRDY is zero
        push    ds
        pop     es
        mov     di,OFFSET cd_sense
        xor     ecx,ecx
        call    cd_command
        pop     es
        pop     di
        pop     ecx
        pop     eax
        ret

cd_pktzero:
        mov     dword ptr [cd_pkt],0
        mov     dword ptr [cd_pkt+4],0
        mov     dword ptr [cd_pkt+8],0
        ret

;=====================================================================
; cd_command - the packet in cd_pkt to the drive; ES:DI = where data
;              goes, ECX = how much will be taken.  -> EAX = bytes that
;              came.  CF if the drive said no or stopped answering, and
;              then cd_skey is what REQUEST SENSE said (0 if nothing).
;              Registers kept but EAX and the flags.
; cd_packet  - the same without the REQUEST SENSE.
;
; A BURST LONGER THAN THE BUFFER IS DRAINED, NOT LEFT: data still in
; the drive wedges the next command, which after a failure is REQUEST
; SENSE (the kernel's atapi_packet says the same).
;=====================================================================
cd_command:
        mov     byte ptr [cd_skey],0
        call    cd_packet
        jnc     ccmd_out
        push    eax
        push    ecx
        push    di
        push    es
        mov     eax,dword ptr [cd_pkt]  ; the command, kept for a retry
        mov     dword ptr [cd_pktsave],eax
        mov     eax,dword ptr [cd_pkt+4]
        mov     dword ptr [cd_pktsave+4],eax
        mov     eax,dword ptr [cd_pkt+8]
        mov     dword ptr [cd_pktsave+8],eax
        call    cd_pktzero
        mov     byte ptr [cd_pkt+0],OP_SENSE
        mov     byte ptr [cd_pkt+4],18
        push    ds
        pop     es
        mov     di,OFFSET cd_sense
        mov     ecx,18
        call    cd_packet
        jc      ccmd_nosense
        cmp     eax,3
        jb      ccmd_nosense
        mov     al,[cd_sense+2]
        and     al,0Fh
        mov     [cd_skey],al
ccmd_nosense:
        mov     eax,dword ptr [cd_pktsave]
        mov     dword ptr [cd_pkt],eax
        mov     eax,dword ptr [cd_pktsave+4]
        mov     dword ptr [cd_pkt+4],eax
        mov     eax,dword ptr [cd_pktsave+8]
        mov     dword ptr [cd_pkt+8],eax
        pop     es
        pop     di
        pop     ecx
        pop     eax
        stc
ccmd_out:
        ret

cd_packet:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    di
        mov     [cd_left],ecx
        mov     dword ptr [cd_got],0
        ; select the drive, and let it settle
        mov     dx,[cd_ctrl]
        mov     al,CTL_NIEN
        out     dx,al
        mov     dx,[cd_base]
        add     dx,ATA_DEV
        mov     al,0A0h
        or      al,[cd_sel]
        out     dx,al
        call    cd_settle
        call    cd_notbusy
        jc      cpk_fail
        mov     dx,[cd_base]
        inc     dx                      ; features: PIO
        xor     al,al
        out     dx,al
        add     dx,ATA_BCLO - ATA_ERR
        mov     al,BCL AND 0FFh
        out     dx,al
        inc     dx
        mov     al,BCL SHR 8
        out     dx,al
        add     dx,ATA_STAT - ATA_BCHI
        mov     al,CMD_PACKET
        out     dx,al
        call    cd_settle
        call    cd_drq                  ; -> AL = the status, DRQ: it wants
        jc      cpk_fail                ; the packet
        test    al,ST_ERR
        jnz     cpk_fail
        mov     dx,[cd_base]
        mov     si,OFFSET cd_pkt
        mov     cx,6
cpk_out:
        lodsw
        out     dx,ax
        loop    cpk_out
cpk_phase:
        call    cd_settle
        call    cd_notbusy
        jc      cpk_fail
        test    al,ST_ERR
        jnz     cpk_fail
        test    al,ST_DRQ
        jz      cpk_done
        ; a burst: its size from the byte count registers
        mov     dx,[cd_base]
        add     dx,ATA_BCHI
        in      al,dx
        mov     ah,al
        dec     dx
        in      al,dx
        movzx   ebx,ax                  ; EBX = bytes in this burst
        inc     ebx
        and     bl,0FEh                 ; words
        mov     dx,[cd_base]
        mov     ecx,[cd_left]
        cmp     ecx,ebx
        jbe     cpk_take
        mov     ecx,ebx
cpk_take:
        sub     [cd_left],ecx
        add     [cd_got],ecx
        sub     ebx,ecx                 ; EBX = what will not fit
        shr     ecx,1
        jcxz    cpk_drain
        rep     insw                    ; ES:DI
cpk_drain:
        shr     ebx,1
        jz      cpk_phase
cpk_drop:
        in      ax,dx
        dec     ebx
        jnz     cpk_drop
        jmp     short cpk_phase
cpk_done:
        mov     dx,[cd_ctrl]
        mov     al,CTL_INTS
        out     dx,al
        mov     eax,[cd_got]
        pop     di
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
cpk_fail:
        mov     dx,[cd_ctrl]
        mov     al,CTL_INTS
        out     dx,al
        mov     eax,[cd_got]
        pop     di
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;--- cd_settle - the four reads of the alternate status that make the
;--- 400ns a device needs after a select or a command
cd_settle:
        push    ax
        push    dx
        mov     dx,[cd_ctrl]
        in      al,dx
        in      al,dx
        in      al,dx
        in      al,dx
        pop     dx
        pop     ax
        ret

;--- cd_drq - after the PACKET command: wait for BSY to go and then for
;--- DRQ or ERR, the drive asking for the twelve bytes.  BSY and DRQ
;--- are meant to change together, and a drive that lets BSY go first
;--- gets a little longer.  -> AL = the status; CF if neither came.
cd_drq:
        call    cd_notbusy
        jc      cdq_out
        push    cx
        push    dx
        mov     dx,[cd_base]
        add     dx,ATA_STAT
        mov     cx,10000
cdq_poll:
        test    al,ST_DRQ OR ST_ERR
        jnz     cdq_have
        in      al,dx
        loop    cdq_poll
        pop     dx
        pop     cx
        stc
        ret
cdq_have:
        pop     dx
        pop     cx
        clc
cdq_out:
        ret

;--- cd_notbusy - wait for BSY to go, ten seconds at most.  -> AL = the
;--- status, CF if it never went.  Interrupts on while it waits: the
;--- tick count is the clock.
cd_notbusy:
        push    ebx
        push    ecx
        push    dx
        sti
        call    cd_ticks
        mov     ebx,eax
        mov     ecx,4000000h            ; and a count, in case the ticks
cnb_poll:                               ; do not move
        mov     dx,[cd_base]
        add     dx,ATA_STAT
        in      al,dx
        test    al,ST_BSY
        jz      cnb_ok
        dec     ecx
        jz      cnb_late
        test    cl,0FFh
        jnz     cnb_poll
        push    eax
        call    cd_ticks
        sub     eax,ebx
        cmp     eax,WAITTICKS
        pop     eax
        jb      cnb_poll
cnb_late:
        stc
        jmp     short cnb_out
cnb_ok:
        clc
cnb_out:
        pop     dx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; c2f_cdx - INT 2Fh AH=15h, the CD-ROM extensions, as MSCDEX 2.23
;           answers them for the one letter there is.  Calls about a
;           letter that is not ours say "invalid drive"; calls this
;           does not do go on down the chain.
;=====================================================================
c2f_cdx:
        cmp     al,00h
        je      cdx_count
        cmp     al,0Bh
        je      cdx_check
        cmp     al,0Ch
        je      cdx_version
        cmp     al,0Dh
        je      cdx_letters
        cmp     al,02h
        jb      cdx_chain
        cmp     al,05h
        jb      cdx_names               ; 02h-04h
        je      cdx_vtoc
        cmp     al,08h
        je      cdx_absread
cdx_chain:
        jmp     dword ptr cs:[cd_old2f]

;--- 1500h: BX = how many CD-ROM letters, CX = the first
cdx_count:
        mov     bx,1
        movzx   cx,byte ptr cs:[cd_letter]
        iret

;--- 150Bh: CX = a letter.  AX nonzero if it is a CD-ROM; BX = ADADh
cdx_check:
        mov     bx,0ADADh
        xor     ax,ax
        cmp     cl,byte ptr cs:[cd_letter]
        jne     cdx_ckout
        or      ch,ch
        jnz     cdx_ckout
        dec     ax
cdx_ckout:
        iret

;--- 150Ch: BX = the version, 2.23
cdx_version:
        mov     bx,0217h
        iret

;--- 150Dh: ES:BX = where the letters go, a byte each
cdx_letters:
        push    ax
        mov     al,byte ptr cs:[cd_letter]
        mov     es:[bx],al
        pop     ax
        iret

;--- the rest are about a letter, in CX, and read the disc --------------
cdx_ours:
        cmp     cl,byte ptr cs:[cd_letter]
        jne     cdx_bad
        or      ch,ch
        jnz     cdx_bad
        ret
cdx_bad:
        pop     ax                      ; our return address
        mov     ax,ERR_BADDRIVE
        stc
        retf    2

;--- 1502h-1504h: the copyright, abstract and bibliographic file names,
;--- 37 characters and a NUL, into ES:BX
cdx_names:
        call    cdx_ours
        sti
        cld
        push    ds
        push    si
        push    di
        push    cx
        push    cs
        pop     ds
        push    ax
        call    cd_ready
        pop     cx                      ; CL = the function
        jc      cdn_fail
        mov     si,PVD_COPYR
        cmp     cl,03h
        jb      cdn_which
        mov     si,PVD_ABSTR
        je      cdn_which
        mov     si,PVD_BIBLIO
cdn_which:
        mov     eax,[cd_pvdblk]
        call    cd_getblk
        jc      cdn_rfail
        add     si,OFFSET cd_blk
        mov     di,bx
        mov     cx,37
        rep     movsb
        mov     byte ptr es:[di],0
        pop     cx
        pop     di
        pop     si
        pop     ds
        clc
        retf    2
cdn_rfail:
        mov     ax,ERR_READFAULT
cdn_fail:
        pop     cx
        pop     di
        pop     si
        pop     ds
        stc
        retf    2

;--- 1505h: DX = which volume descriptor, ES:BX = where its block goes.
;--- AX = 1 for the primary one, FFh for the end of them, 0 otherwise.
cdx_vtoc:
        call    cdx_ours
        sti
        cld
        push    ds
        push    di
        push    cx
        push    cs
        pop     ds
        call    cd_ready
        jc      cvt_fail
        movzx   eax,dx
        add     eax,PVD_BLK
        mov     di,bx
        mov     cx,1
        call    cd_readblks
        jc      cvt_rfail
        movzx   ax,byte ptr es:[bx]
        cmp     al,1
        je      cvt_out
        cmp     al,0FFh
        je      cvt_out
        xor     ax,ax
cvt_out:
        pop     cx
        pop     di
        pop     ds
        clc
        retf    2
cvt_rfail:
        mov     ax,ERR_READFAULT
cvt_fail:
        pop     cx
        pop     di
        pop     ds
        stc
        retf    2

;--- 1508h: SI:DI = the first block, DX = how many, ES:BX = where.  A
;--- block at a time, so that ES:BX may be anywhere.
cdx_absread:
        call    cdx_ours
        sti
        cld
        push    ds
        push    bx
        push    cx
        push    dx
        push    di
        push    es
        push    cs
        pop     ds
        call    cd_ready
        jc      cab_fail
        mov     ax,si
        shl     eax,16
        mov     ax,di                   ; EAX = SI:DI
        mov     di,bx
cab_one:
        or      dx,dx
        jz      cab_done
        mov     cx,1
        call    cd_readblks
        jc      cab_rfail
        inc     eax
        dec     dx
        mov     bx,es                   ; on a paragraph further: 2048
        add     bx,BLK / 16             ; bytes is 128 of them
        mov     es,bx
        jmp     short cab_one
cab_done:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ds
        clc
        retf    2
cab_rfail:
        mov     ax,ERR_READFAULT
cab_fail:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ds
        stc
        retf    2

;=====================================================================
; Data
;=====================================================================
CD_NAMEMAX      EQU     64              ; an identifier is cut to this
cd_old2f    dd  0
cd_lowseg   dw  0
cd_fn       db  0
cd_jmp      dw  0
cd_mounted  db  0
cd_gen      dw  0                       ; discs mounted so far
cd_asked    dd  0                       ; when the drive was last asked
cd_skey     db  0                       ; REQUEST SENSE's sense key
cd_tries    db  0
cd_pvdblk   dd  0
cd_volblks  dd  0
cd_rootext  dd  0
cd_rootsize dd  0
cd_label    db  11 dup(' ')
cd_okch     db  '!#$%&()-@^_{}~'''
CD_OKCHLEN  EQU     $ - cd_okch
; a search, a walk, a read
cd_sattr    db  0
cd_wantlast db  0
cd_namep    dw  0
cd_dirext   dd  0
cd_dirsize  dd  0
cd_pos      dd  0
cd_fcb      db  11 dup(' ')             ; the name, or pattern, being looked for
cd_ename    db  11 dup(' ')             ; a record's 8.3 spelling
cd_name     db  CD_NAMEMAX + 1 dup(0)   ; a record's identifier as text
cd_rext     dd  0                       ; the record in hand
cd_rsize    dd  0
cd_rflags   db  0
cd_rdate    dw  0
cd_rtime    dw  0
cd_sftoff   dw  0                       ; a read: the entry,
cd_sftseg   dw  0
cd_dst      dw  0                       ; where the data goes,
cd_dstseg   dw  0
cd_want     dd  0                       ; how much,
cd_done     dd  0                       ; and how much has gone
; the drive
cd_pkt      db  12 dup(0)
cd_pktsave  db  12 dup(0)
cd_left     dd  0
cd_got      dd  0
cd_sense    db  18 dup(0)
cd_blklba   dd  0FFFFFFFFh              ; the block in cd_blk
        EVEN
cd_blk      db  BLK dup(0)

_RC     ENDS
        END
