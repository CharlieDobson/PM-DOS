;=====================================================================
; RMDISK.INC - sectors, BPBs, and the calls that name a drive
;
; PM-DOS scanned the partition tables and worked out which volume each
; letter meant; RMDOS inherits that answer through RMH_DRIVES rather
; than scanning again.  What it does NOT inherit is the geometry -
; cluster size, where the FAT is, how big the root is - because all of
; that is in the volume's own boot sector, and reading it is less code
; than marshalling it across the handover in a structure both sides
; would then have to agree about forever.
;=====================================================================

VOL_BIOS        EQU 00h                 ; db  BIOS drive number
VOL_FLAGS       EQU 01h                 ; db  VF_*
VOL_START       EQU 02h                 ; dd  absolute LBA of the boot sector
VOL_SPC         EQU 06h                 ; db  sectors per cluster
VOL_FATTYPE     EQU 07h                 ; db  12, 16 or 32
VOL_NFATS       EQU 08h                 ; db
VOL_MEDIA       EQU 09h                 ; db
VOL_RSVD        EQU 0Ah                 ; dw  reserved sectors
VOL_ROOTENT     EQU 0Ch                 ; dw  root entries, 0 on FAT32
VOL_SPT         EQU 0Eh                 ; dw  sectors per track
VOL_HEADS       EQU 10h                 ; dw
VOL_SPF         EQU 12h                 ; dd  sectors per FAT
VOL_TOTSEC      EQU 16h                 ; dd
VOL_FATLBA      EQU 1Ah                 ; dd  absolute, first FAT
VOL_ROOTLBA     EQU 1Eh                 ; dd  absolute, FAT12/16 root
VOL_DATALBA     EQU 22h                 ; dd  absolute, cluster 2
VOL_CLUSTERS    EQU 26h                 ; dd  data clusters; the last is +1
VOL_ROOTCLUS    EQU 2Ah                 ; dd  FAT32 only
VOL_FREEHINT    EQU 2Eh                 ; dd  where to start looking
VOL_DRVHDR      EQU 32h                 ; dd  VF_DRIVER: the driver's header
VOL_UNIT        EQU 36h                 ; db  ...and its number for the unit
VOL_TICK        EQU 38h                 ; dd  VF_FLOPPY: the BIOS tick count
                                        ;     when it was last asked for
VOL_LEN         EQU 3Ch

VOL_SAME_TICKS  EQU 36                  ; two seconds of them - see rd_getvol

VF_PRESENT      EQU 01h
VF_FLOPPY       EQU 02h
VF_MOUNTED      EQU 04h
VF_LBA          EQU 08h                 ; INT 13h extensions are there
VF_DRIVER       EQU 10h                 ; a unit of a driver CONFIG.SYS loaded:
                                        ; its sectors come through request
                                        ; packets, not INT 13h (RMDEV.INC)
VF_STALE        EQU 20h                 ; somebody else may have written it:
                                        ; a redirector held the letter, or
                                        ; SMARTDRV's interface was told to
                                        ; stop or start caching it, which
                                        ; IFSMGR.386 does round VFAT's use of
                                        ; the disk.  rd_getvol forgets what
                                        ; is held of it and mounts it again
                                        ; (vol_forget)
VF_NOWRITE      EQU 40h                 ; the cache keeps no writes for it -
VF_NOREAD       EQU 80h                 ; and no reads.  SMARTDRV's two bits,
                                        ; in SMARTDRV's places: INT 2Fh 4A10h
                                        ; BX=3 answers the byte as it stands.
                                        ; See RMCACHE.INC

SECSIZE         EQU 512

;--- a drive parameter block, MS-DOS 4's 21h bytes (DOS6SRC inc/dpb.inc),
;--- one a drive, in the block below 64K at RL_DPB (RMDOS.INC)
DPB_DRIVE       EQU 00h                 ; db  0 = A
DPB_UNIT        EQU 01h                 ; db  the unit, in rd_dskhdr's terms
DPB_BPS         EQU 02h                 ; dw
DPB_CLMASK      EQU 04h                 ; db  sectors a cluster, less one
DPB_CLSHIFT     EQU 05h                 ; db  ...as a shift
DPB_RSVD        EQU 06h                 ; dw
DPB_NFATS       EQU 08h                 ; db
DPB_ROOTENT     EQU 09h                 ; dw
DPB_DATA        EQU 0Bh                 ; dw  cluster 2's sector, in the volume
DPB_MAXCL       EQU 0Dh                 ; dw  the highest cluster
DPB_SPF         EQU 0Fh                 ; dw
DPB_DIR         EQU 11h                 ; dw  the root's first sector
DPB_DRIVER      EQU 13h                 ; dd  the block driver's header
DPB_MEDIA       EQU 17h                 ; db
DPB_ACCESS      EQU 18h                 ; db  FFh: not read yet, or to be
                                        ;     read again - see rd_getvol
DPB_NEXT        EQU 19h                 ; dd  FFFF:FFFF ends the chain
DPB_FREEHINT    EQU 1Dh                 ; dw
DPB_FREE        EQU 1Fh                 ; dw  FFFFh, not counted

;=====================================================================
; rd_volinit - copy the drive letters PM-DOS handed over into our own
;              table.  Geometry is left until something asks for it: a
;              floppy that is not in the machine must not make the
;              handover fail, and a drive nobody touches need never be
;              read at all.
;=====================================================================
rd_volinit:
        pusha
        push    es
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rd_vols
        mov     cx,LASTDRIVE * VOL_LEN
        xor     al,al
        cld
        rep     stosb

        movzx   cx,byte ptr [h_ndrv]
        or      cx,cx
        jz      rvi_done
        cmp     cx,LASTDRIVE
        jbe     rvi_ok
        mov     cx,LASTDRIVE
rvi_ok:
        mov     si,OFFSET h_drives
        mov     di,OFFSET rd_vols
rvi_one:
        mov     al,[si+RMD_BIOS]
        mov     [di+VOL_BIOS],al
        mov     al,[si+RMD_FLAGS]
        and     al,RMDF_PRESENT OR RMDF_FLOPPY  ; the two bits that mean the
        mov     [di+VOL_FLAGS],al               ; same thing on both sides
        mov     eax,dword ptr [si+RMD_START]
        mov     dword ptr [di+VOL_START],eax
        ; A DRIVER'S UNIT: the start is its header, the unit is in the
        ; flags' top four bits, and the volume starts at its sector 0
        ; (RMDOS.INC, RMDF_DRIVER).
        test    byte ptr [si+RMD_FLAGS],RMDF_DRIVER
        jz      rvi_bios
        or      byte ptr [di+VOL_FLAGS],VF_DRIVER
        mov     dword ptr [di+VOL_DRVHDR],eax
        mov     dword ptr [di+VOL_START],0
        mov     al,[si+RMD_FLAGS]
        shr     al,RMDF_UNITSHIFT
        mov     [di+VOL_UNIT],al
rvi_bios:
        add     si,RMD_LEN
        add     di,VOL_LEN
        loop    rvi_one
rvi_done:
        pop     es
        popa
        ret

;--- rd_ticks - EAX = the BIOS tick count, 18.2 a second, from the BIOS
;--- data area; under WIN386 each VM's copy of it, which all keep time.
rd_ticks:
        push    es
        mov     ax,40h
        mov     es,ax
        mov     eax,dword ptr es:[6Ch]
        pop     es
        ret

;=====================================================================
; rd_getvol - AL = drive, 0 = A -> SI = its record, CF set if there is
;             no such letter or its medium cannot be read.
;
; A FLOPPY IS RE-MOUNTED EVERY TIME it has been left alone for two
; seconds.  There is no change-line logic here and there should not
; be: an excursion is short, one boot sector costs nothing beside being
; wrong about which disk is in the drive, and PM-DOS's own answer to
; this question took three releases.
;
; THE TWO SECONDS ARE MS-DOS'S OWN RULE for a drive with no change
; line: a diskette asked for again within two seconds of the last time
; is the same diskette, because nobody has ever swapped one faster.
; Without it every sector of a file read from A: cost three BIOS
; transfers, two of them the boot sector on track 0 with the head
; travelling there and back - and a DOS box under a floppy boot loads
; COMMAND.COM from A:, fifty-six sectors, a hundred and seventy
; transfers (2026-10-08, the floppy boot on real hardware).  The stamp
; is refreshed on every access, so a program reading steadily never
; pays for the boot sector again; the count wraps at midnight and the
; subtraction comes out huge, which is one spare mount a day.
;=====================================================================
rd_getvol:
        push    ax
        push    dx
        cmp     al,LASTDRIVE
        jae     rgv_bad
        push    ax
        movzx   si,al
        mov     ax,VOL_LEN
        mul     si
        mov     si,ax
        pop     ax
        add     si,OFFSET rd_vols
        test    byte ptr [si+VOL_FLAGS],VF_PRESENT
        jz      rgv_bad
        ; A REDIRECTOR'S FOR NOW, OR BACK FROM ONE.  While the letter is
        ; somebody else's the volume is only marked: what this DOS still
        ; reads of it is the files it opened before (WIN386.EXE, for one),
        ; and their clusters have not moved.  Once the letter is this
        ; DOS's again, whatever it held of the disk is older than the
        ; disk, and goes.
        call    net_drive               ; NZ: a redirector has the letter
        jz      rgv_ours
        or      byte ptr [si+VOL_FLAGS],VF_STALE
        jmp     short rgv_held
rgv_ours:
        ; AND A PARAMETER BLOCK MARKED "NOT ACCESSED" - FFh in its access
        ; byte - is MS-DOS's own way of being told the same thing: a
        ; program that has changed a disk behind DOS's back writes it, and
        ; DOS builds the block, and its idea of the disk, again.
        call    dpb_unread
        test    byte ptr [si+VOL_FLAGS],VF_STALE
        jz      rgv_held
        call    vol_forget
rgv_held:
        test    byte ptr [si+VOL_FLAGS],VF_MOUNTED
        jz      rgv_mount
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jz      rgv_ok
        push    eax                     ; a diskette: the same one if it
        push    edx                     ; was asked for two seconds ago
        call    rd_ticks
        mov     edx,eax
        sub     edx,dword ptr [si+VOL_TICK]
        cmp     edx,VOL_SAME_TICKS
        ja      rgv_older
        mov     dword ptr [si+VOL_TICK],eax
        pop     edx
        pop     eax
        jmp     short rgv_ok
rgv_older:
        pop     edx
        pop     eax
rgv_mount:
IFDEF RMDOS_MARK
        push    ax
        add     al,'A'                  ; which drive is being mounted, which
        call    rd_vmark                ; is the one thing rd_mount cannot say
        pop     ax
ENDIF
        call    rd_mount
        jc      rgv_bad
        call    dpb_fill                ; ...and the drive's parameter block
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jz      rgv_ok
        push    eax
        call    rd_ticks                ; when this diskette was last seen
        mov     dword ptr [si+VOL_TICK],eax
        pop     eax
rgv_ok:
        pop     dx
        pop     ax
        clc
        ret
rgv_bad:
        pop     dx
        pop     ax
        stc
        ret

;=====================================================================
; dpb_fill - AL = a drive, SI = its volume, just mounted: the drive's
;            parameter block out of the volume record, and its access
;            byte 0, "read".  The chain and driver pointers are
;            rd_sysinit's and stay.  Every register is kept.
; dpb_unread - AL = a drive, SI = its volume: if it is mounted and its
;            block's access byte says FFh, VF_STALE.  Registers kept.
;
; WINDOWS FOR WORKGROUPS' IFSMGR.386 ASKS 32h FOR EVERY HARD DISK, and a
; drive with no parameter block is not one it will take for 32-bit file
; access; it then reads the header the block's driver pointer names, to
; see whether the drive is a Stacker or DoubleSpace volume (DOS6SRC
; has the block's shape in inc/dpb.inc).  The fields are DOS 4's and so
; sixteen bits each, which a FAT16 volume fits and FAT32 does not -
; 32h refuses a FAT32 drive, as MS-DOS 7.1's does.
;=====================================================================
dpb_fill:
        pushad
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        movzx   di,al
        imul    di,di,RL_DPBLEN
        add     di,RL_DPB
        mov     es:[di+DPB_DRIVE],al
        mov     es:[di+DPB_UNIT],al
        mov     word ptr es:[di+DPB_BPS],SECSIZE
        mov     al,[si+VOL_SPC]
        dec     al
        mov     es:[di+DPB_CLMASK],al
        mov     al,[si+VOL_SPC]
        xor     cl,cl
dfl_shift:
        shr     al,1
        jz      dfl_shifted
        inc     cl
        jmp     short dfl_shift
dfl_shifted:
        mov     es:[di+DPB_CLSHIFT],cl
        mov     ax,[si+VOL_RSVD]
        mov     es:[di+DPB_RSVD],ax
        mov     al,[si+VOL_NFATS]
        mov     es:[di+DPB_NFATS],al
        mov     ax,[si+VOL_ROOTENT]
        mov     es:[di+DPB_ROOTENT],ax
        mov     eax,dword ptr [si+VOL_DATALBA]
        sub     eax,dword ptr [si+VOL_START]
        mov     es:[di+DPB_DATA],ax
        mov     eax,dword ptr [si+VOL_CLUSTERS]
        inc     eax
        mov     es:[di+DPB_MAXCL],ax
        mov     eax,dword ptr [si+VOL_SPF]
        mov     es:[di+DPB_SPF],ax
        mov     eax,dword ptr [si+VOL_ROOTLBA]
        sub     eax,dword ptr [si+VOL_START]
        mov     es:[di+DPB_DIR],ax
        mov     al,[si+VOL_MEDIA]
        mov     es:[di+DPB_MEDIA],al
        mov     byte ptr es:[di+DPB_ACCESS],0
        mov     eax,dword ptr [si+VOL_FREEHINT]
        mov     es:[di+DPB_FREEHINT],ax
        mov     word ptr es:[di+DPB_FREE],0FFFFh
        pop     es
        popad
        ret

dpb_unread:
        test    byte ptr [si+VOL_FLAGS],VF_MOUNTED
        jz      dur_out
        push    di
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        movzx   di,al
        imul    di,di,RL_DPBLEN
        cmp     byte ptr es:[RL_DPB + di + DPB_ACCESS],0FFh
        pop     es
        pop     di
        jne     dur_out
        or      byte ptr [si+VOL_FLAGS],VF_STALE
dur_out:
        ret

;=====================================================================
; rd_dskhdr - the block driver every parameter block names.  There is
; no block driver in RMDOS: its disks are the BIOS's, read in
; RMDISK.INC itself.  But DOS's blocks always name one - IO.SYS's - and
; what reads a block reads the header too (IFSMGR, above), so this is a
; header, with a unit for each drive.  A request made through it is
; answered "unknown command", done, rather than left for its caller to
; wait on.
;=====================================================================
rd_dskhdr   dd  0FFFFFFFFh              ; in no chain
            dw  0842h                   ; block; 32-bit sectors, generic
                                        ; IOCTL, open and close
            dw  OFFSET dsk_strat
            dw  OFFSET dsk_intr
            db  LASTDRIVE               ; the units
            db  'RMDISK '
dsk_req     dd  0

dsk_strat:
        mov     word ptr cs:[dsk_req],bx
        mov     word ptr cs:[dsk_req+2],es
        retf
dsk_intr:
        push    ds
        push    bx
        lds     bx,dword ptr cs:[dsk_req]
        mov     word ptr [bx+3],8103h   ; error, done, unknown command
        pop     bx
        pop     ds
        retf

;=====================================================================
; 1Fh / 32h - a drive's parameter block: 1Fh the current drive's, 32h
;             the one in DL (0 = the current one).  -> AL = 0 and DS:BX
;             = the block, mounted and filled in now; AL = FFh for a
;             drive that is not there, cannot be read, is a redirector's
;             (as DOS answers one: DOS6SRC dos/misc.asm, $GET_DPB), or
;             is FAT32.  No carry either way.
;=====================================================================
fn1F:
        xor     al,al
        jmp     short f32_which
fn32:
        mov     al,byte ptr [bp+F_DX]
f32_which:
        or      al,al
        jnz     f32_named
        mov     al,[rd_drive]
        inc     al
f32_named:
        dec     al
        call    net_drive
        jnz     f32_none
        call    rd_getvol               ; mounted, and the block filled
        jc      f32_none
        cmp     byte ptr [si+VOL_FATTYPE],32
        je      f32_none
        movzx   bx,al
        imul    bx,bx,RL_DPBLEN
        add     bx,RL_DPB
        mov     [bp+F_BX],bx
        mov     ax,[rd_lowseg]
        mov     [bp+F_DS],ax
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok
f32_none:
        mov     byte ptr [bp+F_AX],0FFh
        jmp     i21_ok

;=====================================================================
; vol_forget - SI = a volume that another file system may have written
;              behind this DOS's back (VF_STALE).  The FAT window and the
;              directory sector, if they are this volume's, are written
;              if this DOS changed them and then let go of; the sector
;              cache drops the disk; and the volume is unmounted, so
;              rd_getvol reads its boot sector again.  Registers kept.
;
; WINDOWS FOR WORKGROUPS' VFAT is the other file system.  With 32-bit
; file access on, IFSMGR.386 takes a hard disk's letter (RMNET.INC) and
; VFAT writes its FAT and directories straight through the disk driver
; in protected mode; when Windows ends the letter comes back, and every
; sector this DOS still held of that disk is the one from before.
; Writing what was dirty first, rather than throwing it away, is for
; the one case where this is reached in the middle of a call (a file of
; this DOS's own being written when the mark arrived): there the buffer
; is this DOS's newest copy and the write is owed.
;=====================================================================
vol_forget:
        push    ax
        cmp     si,[rd_fatvol]
        jne     vfg_dir
        call    fat_flush
        mov     word ptr [rd_fatvol],0
        mov     dword ptr [rd_fatlba],-1
vfg_dir:
        cmp     si,[rd_dirvol]
        jne     vfg_cache
        call    dir_flush
        mov     word ptr [rd_dirvol],0
        mov     dword ptr [rd_dirlba],-1
vfg_cache:
        mov     al,[si+VOL_BIOS]
        call    cm_dropdrive
        and     byte ptr [si+VOL_FLAGS],NOT (VF_STALE OR VF_MOUNTED) AND 0FFh
        pop     ax
        ret

;=====================================================================
; rd_mount - SI = a volume record with BIOS and START filled in.  Read
;            its boot sector and work out the rest.  Every register is
;            kept, all thirty-two bits of it.
;
; ALL THIRTY-TWO BITS, BECAUSE A DISKETTE IS MOUNTED IN THE MIDDLE OF A
; READ.  rd_getvol mounts a diskette every time it is asked for one,
; and file_io and sft_clus ask in the middle of a transfer, holding a
; file position in EAX and the bytes still to move in ECX.  This used
; to save them with PUSHA, which keeps sixteen bits, and then worked in
; EAX, ECX and EDX: the top half of the position came back as nought
; and every file on a diskette wrapped at 64K - the sector at 64K was
; read from the sector at 0, and a write past 64K went over the start
; of the file.  A hard disk is mounted once, at its first use, and
; never showed it.
;=====================================================================
rd_mount:
        pushad
        push    es

        ; A DRIVER'S UNIT HAS NO BIOS TO ASK.  The geometry below is the
        ; CHS path's, which its sectors do not take (rd_drvraw).
        test    byte ptr [si+VOL_FLAGS],VF_DRIVER
        jnz     rm_geodef

        ; Do the extensions exist on this drive?  Asked once per mount
        ; rather than once per read, and asked of the BIOS rather than
        ; guessed from the drive number: a machine old enough to want
        ; RMDOS is old enough to have a BIOS that says no.
        and     byte ptr [si+VOL_FLAGS],NOT VF_LBA
        mov     ah,41h
        mov     bx,55AAh
        mov     dl,[si+VOL_BIOS]
        push    si
        int     13h
        pop     si
        jc      rm_nolba
        cmp     bx,0AA55h
        jne     rm_nolba
        test    cl,1                            ; packet access
        jz      rm_nolba
        or      byte ptr [si+VOL_FLAGS],VF_LBA
rm_nolba:

        ; The geometry the CHS path needs, from the BIOS rather than
        ; from the BPB: a partition's BPB records the geometry of the
        ; disk it was FORMATTED on, and an image moved between machines
        ; carries the old one.  A diskette is the other way about, and
        ; its boot sector is believed once it has been read (below).
        mov     ah,08h
        mov     dl,[si+VOL_BIOS]
        push    si
        xor     di,di
        mov     es,di
        int     13h
        pop     si
        jc      rm_geodef
        movzx   ax,cl
        and     ax,3Fh
        or      ax,ax
        jz      rm_geodef
        mov     [si+VOL_SPT],ax
        movzx   ax,dh
        inc     ax
        mov     [si+VOL_HEADS],ax
        jmp     short rm_geook
rm_geodef:
        mov     word ptr [si+VOL_SPT],18        ; a 1.44M floppy, the only
        mov     word ptr [si+VOL_HEADS],2       ; thing worth guessing
rm_geook:

        mov     ax,cs
        mov     es,ax
        mov     bx,OFFSET rd_bootbuf
        mov     eax,dword ptr [si+VOL_START]
        mov     cx,1
        call    rd_rdsec
        jc      rm_fail
IFDEF RMDOS_MARK
        mov     al,'R'                  ; the boot sector came off the disk
        call    rd_vmark
ENDIF

        mov     di,OFFSET rd_bootbuf
        cmp     word ptr [di+0Bh],SECSIZE
        jne     rm_fail                         ; not 512, and everything
                                                ; below assumes 512
        mov     al,[di+0Dh]
        or      al,al
        jz      rm_fail
        mov     [si+VOL_SPC],al
        mov     ax,[di+0Eh]
        or      ax,ax
        jz      rm_fail
        mov     [si+VOL_RSVD],ax
        mov     al,[di+10h]
        mov     [si+VOL_NFATS],al
        mov     ax,[di+11h]
        mov     [si+VOL_ROOTENT],ax
        mov     al,[di+15h]
        mov     [si+VOL_MEDIA],al

        movzx   eax,word ptr [di+16h]           ; sectors per FAT
        or      eax,eax
        jnz     rm_spf
        mov     eax,dword ptr [di+24h]
rm_spf:
        mov     dword ptr [si+VOL_SPF],eax

        movzx   eax,word ptr [di+13h]           ; total sectors
        or      eax,eax
        jnz     rm_tot
        mov     eax,dword ptr [di+20h]
rm_tot:
        mov     dword ptr [si+VOL_TOTSEC],eax

        ; A DISKETTE'S GEOMETRY IS THE DISK'S, NOT THE DRIVE'S.  AH=08h
        ; above describes the drive: eighteen sectors a track for a
        ; 1.44M drive whatever is in it, so a 720K disk was read from
        ; the wrong sector from logical sector 9 onwards - a sector the
        ; track does not have, or one on the wrong track.  The boot
        ; sector says what the disk is, and is believed when its three
        ; figures agree: so many sectors a track, on one or two sides,
        ; making a whole number of tracks between 40 and 84.  The kernel
        ; has done the same since fm_layout learned it (FAT.INC).  The
        ; boot sector itself is sector 1 of track 0 under any geometry.
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jz      rm_fgdone
        push    eax
        push    bx
        mov     cx,word ptr [di+18h]            ; sectors a track
        mov     bx,word ptr [di+1Ah]            ; sides
        or      cx,cx
        jz      rm_fgkeep
        cmp     cx,63                           ; all CHS has room for
        ja      rm_fgkeep
        or      bx,bx
        jz      rm_fgkeep
        cmp     bx,2
        ja      rm_fgkeep
        mov     ax,cx
        mul     bl                              ; AX = sectors a cylinder
        movzx   ecx,ax
        mov     eax,dword ptr [si+VOL_TOTSEC]
        xor     edx,edx
        div     ecx                             ; EAX = cylinders
        or      edx,edx
        jnz     rm_fgkeep                       ; not a whole number of them
        cmp     eax,40
        jb      rm_fgkeep
        cmp     eax,84
        ja      rm_fgkeep
        mov     ax,word ptr [di+18h]
        mov     [si+VOL_SPT],ax
        mov     [si+VOL_HEADS],bx
rm_fgkeep:
        pop     bx
        pop     eax
rm_fgdone:

        mov     eax,dword ptr [di+2Ch]          ; FAT32's root cluster
        mov     dword ptr [si+VOL_ROOTCLUS],eax

        movzx   eax,word ptr [si+VOL_RSVD]      ; FAT one, absolute
        add     eax,dword ptr [si+VOL_START]
        mov     dword ptr [si+VOL_FATLBA],eax

        movzx   ecx,byte ptr [si+VOL_NFATS]     ; the root, absolute
        mov     eax,dword ptr [si+VOL_SPF]
        mul     ecx
        add     eax,dword ptr [si+VOL_FATLBA]
        mov     dword ptr [si+VOL_ROOTLBA],eax

        movzx   ecx,word ptr [si+VOL_ROOTENT]   ; and where data begins
        shl     ecx,5
        add     ecx,SECSIZE - 1
        shr     ecx,9
        add     eax,ecx
        mov     dword ptr [si+VOL_DATALBA],eax

        mov     eax,dword ptr [si+VOL_TOTSEC]   ; how many data clusters
        add     eax,dword ptr [si+VOL_START]
        sub     eax,dword ptr [si+VOL_DATALBA]
        jbe     rm_fail
        movzx   ecx,byte ptr [si+VOL_SPC]
        xor     edx,edx
        div     ecx
        mov     dword ptr [si+VOL_CLUSTERS],eax

        ; THE COUNT DECIDES, NOT THE LABEL.  The string at offset 36h
        ; says "FAT16   " on volumes that are FAT12 and the other way
        ; about; the cluster count is the definition and always was.
        mov     byte ptr [si+VOL_FATTYPE],12
        cmp     eax,4085
        jb      rm_typed
        mov     byte ptr [si+VOL_FATTYPE],16
        cmp     eax,65525
        jb      rm_typed
        mov     byte ptr [si+VOL_FATTYPE],32
rm_typed:
IFDEF RMDOS_MARK
        mov     al,'M'                  ; ...and the BPB behind it made sense
        call    rd_vmark
ENDIF
        mov     dword ptr [si+VOL_FREEHINT],2
        or      byte ptr [si+VOL_FLAGS],VF_MOUNTED
        pop     es
        popad
        clc
        ret
rm_fail:
IFDEF RMDOS_MARK
        mov     al,'X'
        call    rd_vmark
        mov     ax,word ptr [si+VOL_START]      ; the sector we asked for...
        call    rd_vhex
        mov     ax,word ptr [rd_bootbuf+0Bh]    ; ...and the bytes per sector
        call    rd_vhex                         ; in whatever came back
        mov     ax,word ptr [rd_bootbuf]        ; and its first two bytes
        call    rd_vhex
ENDIF
        and     byte ptr [si+VOL_FLAGS],NOT VF_MOUNTED
        pop     es
        popad
        stc
        ret

;=====================================================================
; rd_rdsec / rd_wrsec - EAX = absolute LBA, CX = sectors, ES:BX = the
;                       buffer, SI = the volume record.  CF on error.
;
; Both go through one routine because the only difference is two
; opcodes, and two copies of the CHS arithmetic is how the write path
; ends up being the one that was never tested.
;
; THE CACHE SITS HERE, between the request and the BIOS: cm_xfer
; (RMCACHE.INC) answers what it holds, reads ahead for what it does
; not, and calls rd_raw for the transfers that have to happen.  Every
; sector RMDOS moves comes through this one door - the FAT window, the
; directory sector, file data, INT 25h/26h, a boot sector being
; mounted - which is what makes a cache under it coherent.
;=====================================================================
rd_rdsec:
        push    dx
        mov     dl,0
        jmp     short rd_xfer
rd_wrsec:
        push    dx
        mov     dl,1
rd_xfer:
        pushad
        mov     [rd_rw],dl
        mov     dl,[si+VOL_BIOS]
        mov     [rd_bios],dl
        mov     dx,[si+VOL_SPT]
        mov     [rd_spt],dx
        mov     dx,[si+VOL_HEADS]
        mov     [rd_heads],dx
        mov     dl,[si+VOL_FLAGS]
        mov     [rd_vflags],dl
        mov     edx,dword ptr [si+VOL_DRVHDR]   ; a driver's: the header,
        mov     dword ptr [rd_dhdr],edx         ; the unit and the media
        mov     dl,[si+VOL_UNIT]                ; byte its packets carry
        mov     [rd_dunit],dl
        mov     dl,[si+VOL_MEDIA]
        mov     [rd_dmedia],dl
        movzx   ecx,cx
IFDEF RMDOS_E9
        call    e9_disk                 ; the request, on port E9h
ENDIF
        call    cm_xfer
        jc      rx_dead
rx_ok:
IFDEF RMDOS_E9
        mov     al,[cmv_last]           ; H hit, M miss, W write, . raw
        out     0E9h,al
        mov     al,'+'
        call    e9_diskend
ENDIF
        popad
        pop     dx
        clc
        ret
rx_dead:
IFDEF RMDOS_E9
        mov     al,'-'
        call    e9_diskend
ENDIF
        popad
        pop     dx
        stc
        ret

;=====================================================================
; rd_raw - the BIOS transfer itself: EAX = absolute LBA, ECX = sectors,
;          ES:BX = the buffer, [rd_rw] and the rd_* copies of the
;          volume already set by rd_xfer.  CF on error.  Every
;          register kept, ES INCLUDED: the run arithmetic below
;          renormalises ES:BX as it goes, and a caller that hands
;          over its own buffer gets its own segment back.
;
; AS MANY SECTORS A CALL AS THE BIOS WILL TAKE.  Until v0.63 this made
; one call per sector - simpler, and on a path that ran for seconds it
; did not matter.  It matters now: a Windows start-up is thousands of
; these, and under WIN386 every INT 13h is a trip through the monitor
; whatever it carries.  A run is cut at the end of the track on the
; CHS path, at a 64K boundary in memory on both, and at 127 sectors,
; the smallest limit any BIOS is known to have.
;
;--- THE SECTOR THAT ITSELF STRADDLES A 64K BOUNDARY GOES THROUGH A
;--- BUFFER.  The diskette BIOS moves its data by DMA, and the DMA
;--- controller cannot carry one transfer across a 64K page: the BIOS
;--- checks first, answers 09h and moves nothing.  Until v0.63 such a
;--- sector went alone, as if alone were enough - and for every
;--- transfer that ever happened it was, because the callers' buffers
;--- (RMDOS reads whole sectors straight into them) had never
;--- straddled a page.  Then WIN386 started a DOS box under a floppy
;--- boot, whose COMSPEC was A:\COMMAND.COM: one sector of it landed
;--- across a page, the BIOS said 09h, the reset and retry said it
;--- again, and the box said "Cannot start application" (2026-10-08,
;--- the floppy boot on real hardware).  rd_bounce is two sectors
;--- long so that one half of it is within a page wherever RMDOS was
;--- loaded; a straddler is read into that half and copied out, or
;--- copied in and written from it.  A hard disk's BIOS reads by port
;--- I/O and would not care, but one sector through a buffer costs
;--- nothing, and one rule is one rule.
;
;--- CHS FIRST, AND THE PACKET ONLY WHERE CHS CANNOT REACH.
;
; The extended calls are the modern ones and they are the ones that do
; not work here.  AH=42h against this BIOS RETURNS SUCCESS AND MOVES
; NOTHING: the boot sector read came back with carry clear, AH zero,
; and a buffer still full of the zeroes it was assembled with, so the
; volume failed to mount on a BPB that had never been read.  A failure
; with no symptom is the worst kind, and this is the second time this
; tree has met this exact one - RMDBG.INC gave up on AH=43h for the
; same reason and left a note saying so, and this routine never got
; the message.
;
; So the packet is the FALLBACK, reached only when the cylinder will
; not fit in ten bits.  CHS reaches 1024 x 256 x 63 sectors, which is
; eight gigabytes; a machine that runs Windows 3.1 does not have one,
; and if it ever does, the path that cannot be tested here is at least
; not the path everything depends on.
;=====================================================================
rd_raw:
        test    byte ptr [rd_vflags],VF_DRIVER
        jz      rdr_bios
        jmp     rd_drvraw               ; RMDEV.INC
rdr_bios:
        push    es
        pushad
rdr_one:
        or      ecx,ecx
        jz      rdr_ok
        mov     byte ptr [rd_retry],0
        mov     byte ptr [rd_bounced],0

        ; ES:BX as a flat address and back as a segment with BX below
        ; 16, so that a run's bytes never carry BX past 64K; and how
        ; many sectors fit before the next 64K boundary in memory
        push    eax
        mov     ax,es
        movzx   eax,ax
        shl     eax,4
        movzx   edx,bx
        add     eax,edx                 ; EAX = flat
        mov     edx,eax
        shr     edx,4
        mov     es,dx
        mov     edx,eax
        and     dx,0Fh
        mov     bx,dx
        and     eax,0FFFFh
        neg     eax
        add     eax,10000h              ; bytes to the boundary
        shr     eax,9                   ; ...as whole sectors
        jnz     rdr_bound
        inc     eax                     ; the first one straddles it: alone,
        mov     byte ptr [rd_bounced],1 ; and through rd_bounce
rdr_bound:
        cmp     eax,ecx
        jbe     rdr_bound2
        mov     eax,ecx
rdr_bound2:
        cmp     eax,127
        jbe     rdr_bound3
        mov     eax,127
rdr_bound3:
        mov     [rd_run],eax
        pop     eax
        cmp     byte ptr [rd_bounced],0
        je      rdr_try
        mov     [rd_ubx],bx             ; the straddler: the caller's ES:BX
        mov     [rd_ues],es             ; kept, the BIOS given rd_bounce
        call    rd_bncsel               ; -> ES:BX = the half within a page
        cmp     byte ptr [rd_rw],0
        je      rdr_try
        call    rd_bncin                ; a write: the sector into it first

rdr_try:
        push    eax
        push    ecx
        xor     edx,edx
        movzx   ecx,word ptr [rd_spt]
        div     ecx                     ; EAX = track, EDX = sector - 1
        sub     ecx,edx                 ; sectors left on the track
        cmp     ecx,dword ptr [rd_run]
        jae     rdr_track
        mov     [rd_run],ecx            ; the run ends with the track
rdr_track:
        inc     dl
        mov     [rd_sec],dl
        xor     edx,edx
        movzx   ecx,word ptr [rd_heads]
        div     ecx                     ; EAX = cylinder, EDX = head
        mov     [rd_head],dl
        cmp     eax,1023
        ja      rdr_chsbad
        mov     ch,al
        mov     cl,ah
        shl     cl,6
        or      cl,[rd_sec]
        mov     dh,[rd_head]
        mov     dl,[rd_bios]
        mov     ah,02h
        cmp     byte ptr [rd_rw],0
        je      rdr_chsgo
        mov     ah,03h
rdr_chsgo:
        mov     al,byte ptr [rd_run]
        push    si
        int     13h
        pop     si
        pop     ecx
        pop     eax
        jnc     rdr_next
        jmp     short rdr_err
rdr_chsbad:
        pop     ecx
        pop     eax
        test    byte ptr [rd_vflags],VF_LBA
        jz      rdr_dead                 ; past the tenth bit, and no packet
        mov     word ptr [rd_dap+0],0010h
        mov     dx,word ptr [rd_run]
        mov     word ptr [rd_dap+2],dx
        mov     word ptr [rd_dap+4],bx
        mov     word ptr [rd_dap+6],es
        mov     dword ptr [rd_dap+8],eax
        mov     dword ptr [rd_dap+12],0
        push    eax
        push    ecx
        push    si
        mov     ah,42h
        cmp     byte ptr [rd_rw],0
        je      rdr_lbago
        mov     ax,4300h                ; write, no verify
rdr_lbago:
        mov     dl,[rd_bios]
        mov     si,OFFSET rd_dap
        int     13h
        pop     si
        pop     ecx
        pop     eax
        jnc     rdr_next

;--- ONE RETRY, AND IT IS A RESET.  A floppy that has not been touched
;--- for a while answers the first seek with an error and the second
;--- correctly, which is why every DOS ever written does this and why
;--- leaving it out looks exactly like a bad disk.
rdr_err:
IFDEF RMDOS_E9
        call    e9_diskerr              ; " e=XX", the BIOS's AH
ENDIF
        cmp     byte ptr [rd_retry],0
        jne     rdr_dead
        mov     byte ptr [rd_retry],1
        push    eax
        push    ecx
        push    si
        xor     ah,ah
        mov     dl,[rd_bios]
        int     13h
        pop     si
        pop     ecx
        pop     eax
        jmp     rdr_try

rdr_next:
        cmp     byte ptr [rd_bounced],0
        je      rdr_next2
        cmp     byte ptr [rd_rw],0
        jne     rdr_unbnc
        call    rd_bncout               ; a read: the sector out to the caller
rdr_unbnc:
        mov     bx,[rd_ubx]             ; and the run goes on from the
        mov     es,[rd_ues]             ; caller's address
rdr_next2:
        mov     edx,[rd_run]
        add     eax,edx
        sub     ecx,edx
        shl     dx,9                    ; at most 127 sectors, and BX < 16
        add     bx,dx
        jmp     rdr_one
rdr_ok:
        popad
        pop     es
        clc
        ret
rdr_dead:
        popad
        pop     es
        stc
        ret

;--- rd_bncsel - ES:BX = the half of rd_bounce that lies within one 64K
;--- page.  The buffer is two sectors long and RMDOS is anywhere, so
;--- the first half may straddle a page; the second then begins less
;--- than a sector past it and cannot.
;--- EAX IS THE SECTOR and is kept: the first build of this clobbered
;--- it with the flat address, and the BIOS was then asked for cylinder
;--- 1023 of a diskette - the straddling sector still failed, with a
;--- different error, and a night went on the wrong question.
rd_bncsel:
        push    eax
        mov     ax,cs
        mov     es,ax
        movzx   eax,ax
        shl     eax,4
        add     eax,OFFSET rd_bounce    ; the first half, as a flat address
        mov     bx,OFFSET rd_bounce
        and     eax,0FFFFh
        cmp     eax,10000h - SECSIZE
        jbe     rdb_sel
        add     bx,SECSIZE
rdb_sel:
        pop     eax
        ret

;--- rd_bncin - the caller's sector (rd_ues:rd_ubx) into the bounce half
;--- at ES:BX; rd_bncout - the bounce half at ES:BX (= CS:BX) out to the
;--- caller's.  Registers kept.
rd_bncin:
        push    ds
        push    si
        push    di
        push    cx
        mov     di,bx
        mov     si,[rd_ubx]
        mov     ds,[rd_ues]
        mov     cx,SECSIZE / 4
        cld
        rep     movsd
        pop     cx
        pop     di
        pop     si
        pop     ds
        ret
rd_bncout:
        push    es
        push    si
        push    di
        push    cx
        mov     si,bx
        mov     di,[rd_ubx]
        mov     es,[rd_ues]
        mov     cx,SECSIZE / 4
        cld
        rep     movsd
        pop     cx
        pop     di
        pop     si
        pop     es
        ret

;=====================================================================
; 0Dh - disk reset.  Everything dirty goes out; nothing else happens.
;=====================================================================
fn0D:
        call    rd_flush
        jmp     i21_ok

;=====================================================================
; 0Eh - select a drive.  DL = it -> AL = how many letters there are.
;
; ONLY A LETTER THAT IS IN USE, as MS-DOS has it ($SET_DEFAULT_DRIVE,
; GetVisDrv): a letter with no drive behind it leaves the current one
; where it was, and that is how programs tell the two apart - select
; it and see whether 19h moved.  Windows 3.1 does exactly that for
; GetDriveType, and until this was so every letter below eight was a
; fixed disk to it: File Manager showed D: to H: on a machine with a C:
; and a CD-ROM (Charlie's 486, 2026-10-09).  A redirector's drive is in
; use too, so a mapped network letter can be made current.
;=====================================================================
fn0E:
        mov     al,byte ptr [bp+F_DX]
        call    cds_inuse
        jz      f0E_out
        mov     [rd_drive],al
f0E_out:
        mov     al,[rd_ncds]
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; 19h - which drive is current?  AL = it, 0 = A.
;=====================================================================
fn19:
        mov     al,[rd_drive]
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; 1Ah - set the disk transfer address to DS:DX
; 2Fh - and read it back into ES:BX
;=====================================================================
fn1A:
        mov     ax,[bp+F_DS]
        mov     [rd_dtaseg],ax
        mov     ax,[bp+F_DX]
        mov     [rd_dtaoff],ax
        jmp     i21_ok

fn2F:
        mov     ax,[rd_dtaseg]
        mov     [bp+F_ES],ax
        mov     ax,[rd_dtaoff]
        mov     [bp+F_BX],ax
        jmp     i21_ok

;=====================================================================
; 36h - free space on the drive in DL, 0 = the current one.
;
;   AX = sectors per cluster    BX = free clusters
;   CX = bytes per sector       DX = clusters in all
;
; The free count is A WALK OF THE FAT, because there is nowhere else it
; could come from: FAT16 records no free count anywhere, and the FAT32
; information sector is a hint that is allowed to be wrong.
;=====================================================================
fn36:
        mov     al,byte ptr [bp+F_DX]
        or      al,al
        jnz     f36_named
        mov     al,[rd_drive]
        inc     al
f36_named:
        dec     al
        call    net_drive               ; a redirector's drive: it says
        jnz     f36_net                 ; (110Ch, DOS's DISK_INFO)
        call    rd_getvol
        jc      f36_bad

        movzx   ax,byte ptr [si+VOL_SPC]
        mov     [bp+F_AX],ax
        mov     word ptr [bp+F_CX],SECSIZE
        mov     eax,dword ptr [si+VOL_CLUSTERS]
        cmp     eax,0FFFFh
        jbe     f36_tot
        mov     eax,0FFFFh              ; the field is a word and the disk
f36_tot:                                ; may not be
        mov     [bp+F_DX],ax

        call    rd_countfree            ; SI = volume -> EAX = free
        cmp     eax,0FFFFh
        jbe     f36_free
        mov     eax,0FFFFh
f36_free:
        mov     [bp+F_BX],ax
        jmp     i21_ok
f36_bad:
        mov     word ptr [bp+F_AX],0FFFFh
        jmp     i21_ok                  ; 36h says no with FFFFh, not carry
f36_net:
        call    net_dinfo               ; -> AL, AH, BX, CX, DX
        jc      f36_bad
        xor     ah,ah                   ; AH was the media byte
        mov     [bp+F_AX],ax
        mov     [bp+F_BX],dx            ; free
        mov     [bp+F_CX],cx
        mov     [bp+F_DX],bx            ; in all
        jmp     i21_ok

;=====================================================================
; 1Bh / 1Ch - the shape of a drive: 1Bh the current one, 1Ch the one
; in DL (0 = current).
;
;   AL = sectors per cluster    CX = bytes per sector
;   DX = clusters in all        DS:BX -> the media ID byte
;
; AL = FFh for a drive that is not there, and no carry - the CP/M-era
; way of saying no, which 1Ch kept.  DS:BX points into this DOS, at the
; volume record's own copy of the byte, as DOS pointed into its DPB.
;=====================================================================
fn1B:
        xor     al,al
        jmp     short f1C_which
fn1C:
        mov     al,byte ptr [bp+F_DX]
f1C_which:
        or      al,al
        jnz     f1C_named
        mov     al,[rd_drive]
        inc     al
f1C_named:
        dec     al
        call    net_drive
        jnz     f1C_net
        call    rd_getvol
        jc      f1C_bad
        mov     al,[si+VOL_SPC]
        mov     byte ptr [bp+F_AX],al
        mov     word ptr [bp+F_CX],SECSIZE
        mov     eax,dword ptr [si+VOL_CLUSTERS]
        cmp     eax,0FFFFh
        jbe     f1C_tot
        mov     eax,0FFFFh
f1C_tot:
        mov     [bp+F_DX],ax
        lea     ax,[si+VOL_MEDIA]
        mov     [bp+F_BX],ax
        mov     [bp+F_DS],cs
        jmp     i21_ok
f1C_bad:
        mov     byte ptr [bp+F_AX],0FFh
        jmp     i21_ok
f1C_net:
        ; A redirector's drive: 110Ch's answer, and the media byte it gave
        ; kept here for DS:BX to point at
        call    net_dinfo
        jc      f1C_bad
        mov     byte ptr [bp+F_AX],al
        mov     [bp+F_CX],cx
        mov     [bp+F_DX],bx
        mov     [net_media],ah
        mov     word ptr [bp+F_BX],OFFSET net_media
        mov     [bp+F_DS],cs
        jmp     i21_ok

;=====================================================================
; 69h - the volume serial number, label and file system name, out of
; the extended BPB in the boot sector.  AL = 0 get (BL = drive, 0 =
; current; DS:DX -> info level word, serial dword, eleven-byte label,
; eight-byte type).  AL = 1, set, is refused: nothing on this side of
; the handover rewrites a boot sector.
;=====================================================================
fn69:
        cmp     byte ptr [bp+F_AX],0
        jne     f69_fn
        mov     al,byte ptr [bp+F_BX]
        or      al,al
        jnz     f69_named
        mov     al,[rd_drive]
        inc     al
f69_named:
        dec     al
        call    rd_getvol
        jc      f69_drive
        mov     eax,dword ptr [si+VOL_START]
        push    es
        push    cs
        pop     es
        mov     bx,OFFSET rd_datbuf
        mov     cx,1
        call    rd_rdsec
        pop     es
        jc      f69_access
        mov     bx,26h                  ; FAT12/16's extended BPB
        cmp     byte ptr [si+VOL_FATTYPE],32
        jne     f69_bpb
        mov     bx,42h                  ; FAT32's
f69_bpb:
        cmp     byte ptr [rd_datbuf + bx],29h
        jne     f69_fn                  ; formatted before DOS 4: none to give
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        xor     ax,ax
        cld
        stosw                           ; info level 0
        lea     si,[rd_datbuf + bx + 1]
        mov     cx,4 + 11 + 8           ; serial, label, file system
        rep     movsb
        jmp     i21_ok
f69_drive:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail
f69_access:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f69_fn:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; INT 25h / INT 26h - absolute disk read and write.
;
;   AL = drive, CX = sectors, DX = first sector, DS:BX = buffer
;   or CX = FFFFh and DS:BX -> a ten-byte packet whose sector number
;   is 32 bits, which is the only form that reaches past 32MB.
;
; AND THEY LEAVE THE FLAGS ON THE STACK.  Not a bug in the caller and
; not optional: these two return with the flags word the interrupt
; pushed still there, so the caller sees the carry AND gets its own
; flags back.  Every caller does a POPF afterwards.  Every register
; except the segment registers may be destroyed, which is what makes
; the sprawl below legal.
;=====================================================================
i25:
        mov     byte ptr cs:[rd_absrw],0
        jmp     short i25_go
i26:
        mov     byte ptr cs:[rd_absrw],1
i25_go:
        push    ds
        push    es
        push    bp
        call    crit_in                 ; the sector buffers are one of each

        mov     byte ptr cs:[rd_absdrv],al

        cmp     cx,0FFFFh
        je      i25_packet
        movzx   eax,dx                  ; the short form: a 16-bit sector
        movzx   ecx,cx
        push    ds
        pop     es                      ; ES:BX is the buffer already
        jmp     short i25_have
i25_packet:
        mov     eax,dword ptr [bx]
        movzx   ecx,word ptr [bx+4]
        les     bp,dword ptr [bx+6]
        mov     bx,bp
        mov     bp,es
        mov     es,bp
i25_have:
        mov     dword ptr cs:[rd_abslba],eax
        mov     dword ptr cs:[rd_abscnt],ecx

        mov     ax,cs
        mov     ds,ax
        mov     al,[rd_absdrv]
        call    rd_getvol
        jc      i25_bad

        mov     eax,dword ptr [rd_abslba]
        add     eax,dword ptr [si+VOL_START]
        mov     ecx,dword ptr [rd_abscnt]
        cmp     byte ptr [rd_absrw],0
        je      i25_read
        call    rd_wrsec
        jmp     short i25_done
i25_read:
        call    i25_bios                ; as IO.SYS reads it, if it can be
        jnc     i25_done
        call    rd_rdsec                ; ...and the usual way if not
i25_done:
        jc      i25_bad
        call    crit_out
        pop     bp
        pop     es
        pop     ds
        xor     ax,ax
        clc
        retf                            ; the flags stay pushed - see above
i25_bad:
        call    crit_out
        pop     bp
        pop     es
        pop     ds
        mov     ax,0207h                ; sector not found / general failure
        stc
        retf

;=====================================================================
; i25_bios - INT 25h's read, asked of the BIOS as IO.SYS's disk driver
;            asks it: INT 13h AH=02h, a track's worth at most each time,
;            straight into the caller's buffer at the caller's own ES:BX,
;            and not through the sector cache.  SI = the volume, EAX =
;            the first sector (absolute), ECX = how many, ES:BX = the
;            buffer.  CF if it cannot be done that way - a diskette, a
;            driver's unit, a sector past cylinder 1023, a buffer the
;            transfer would carry past the end of its segment, or the
;            BIOS refusing - and the caller then reads the usual way.
;            Every register is kept.
;
; WHY THE SHAPE MATTERS.  Windows for Workgroups' IFSMGR.386 learns which
; BIOS disk each drive letter is on, and where on it the drive starts, by
; putting a hook of its own on INT 13h and reading each drive's sector 0
; through INT 25h into a buffer at offset 0012h; the read it takes for
; its answer is an AX=0201h whose ES:BX is that buffer, untouched.  The
; cache reads a whole 4K element into memory of its own, and rd_raw
; brings every offset under 16, so neither was ever the read it looked
; for - and a drive IFSMGR could not place is one it will not take for
; 32-bit file access.  The cache writes through, so the disk is never
; behind it, and a read that goes round it reads what it would have.
;=====================================================================
i25_bios:
        pushad
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY OR VF_DRIVER
        jnz     ib_no
        cmp     word ptr [si+VOL_SPT],0
        je      ib_no
        cmp     word ptr [si+VOL_HEADS],0
        je      ib_no
        movzx   edx,bx
        mov     edi,ecx
        shl     edi,9
        add     edx,edi
        cmp     edx,10000h
        ja      ib_no                   ; it would run off the segment
        mov     dword ptr [ib_lba],eax
        mov     dword ptr [ib_left],ecx
ib_more:
        cmp     dword ptr [ib_left],0
        je      ib_done
        mov     eax,dword ptr [ib_lba]
        xor     edx,edx
        movzx   ecx,word ptr [si+VOL_SPT]
        div     ecx                     ; EAX = the track, EDX = the sector
        mov     edi,ecx                 ; in it
        sub     edi,edx                 ; ...and how many are left on it
        inc     dl
        mov     [ib_sec],dl             ; counted from 1
        xor     edx,edx
        movzx   ecx,word ptr [si+VOL_HEADS]
        div     ecx                     ; EAX = the cylinder, EDX = the head
        cmp     eax,1023
        ja      ib_no
        mov     [ib_head],dl
        cmp     edi,dword ptr [ib_left]
        jbe     ib_count
        mov     edi,dword ptr [ib_left]
ib_count:
        mov     ch,al                   ; the cylinder's low eight bits,
        mov     cl,ah                   ; its top two in CL's
        shl     cl,6
        or      cl,[ib_sec]
        mov     dh,[ib_head]
        mov     dl,[si+VOL_BIOS]
        mov     ax,di
        mov     ah,02h
        push    si
        push    di
        int     13h
        pop     di
        pop     si
        jc      ib_no
        movzx   eax,di
        add     dword ptr [ib_lba],eax
        sub     dword ptr [ib_left],eax
        shl     di,9
        add     bx,di
        jmp     short ib_more
ib_done:
        popad
        clc
        ret
ib_no:
        popad
        stc
        ret

;=====================================================================
; Data
;=====================================================================
ib_lba      dd  0                       ; i25_bios: the next sector,
ib_left     dd  0                       ; how many are still to come,
ib_sec      db  0                       ; and where the next one is
ib_head     db  0
rd_vols     db  (LASTDRIVE * VOL_LEN) dup(0)
rd_dhdr     dd  0                       ; rd_xfer's copies of a driver
rd_dunit    db  0                       ; volume's header, unit and media,
rd_dmedia   db  0                       ; for rd_drvraw
rd_bootbuf  db  SECSIZE dup(0)
rd_dap      db  16 dup(0)
rd_bios     db  0
rd_rw       db  0
rd_retry    db  0
rd_vflags   db  0
rd_spt      dw  18
rd_heads    dw  2
rd_run      dd  0                       ; sectors in the BIOS call being made
rd_head     db  0
rd_sec      db  0
rd_absdrv   db  0
rd_absrw    db  0
rd_abslba   dd  0
rd_abscnt   dd  0
rd_bounce   db  (SECSIZE * 2) dup(0)    ; a sector that straddles a 64K
rd_bounced  db  0                       ; page goes through the half of
rd_ubx      dw  0                       ; this within one, the caller's
rd_ues      dw  0                       ; ES:BX kept here meanwhile (rd_raw)
