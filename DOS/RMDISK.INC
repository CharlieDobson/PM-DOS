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
VOL_LEN         EQU 32h

VF_PRESENT      EQU 01h
VF_FLOPPY       EQU 02h
VF_MOUNTED      EQU 04h
VF_LBA          EQU 08h                 ; INT 13h extensions are there

SECSIZE         EQU 512

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
        add     si,RMD_LEN
        add     di,VOL_LEN
        loop    rvi_one
rvi_done:
        pop     es
        popa
        ret

;=====================================================================
; rd_getvol - AL = drive, 0 = A -> SI = its record, CF set if there is
;             no such letter or its medium cannot be read.
;
; A FLOPPY IS RE-MOUNTED EVERY TIME.  There is no change-line logic
; here and there should not be: an excursion is short, one boot sector
; costs nothing beside being wrong about which disk is in the drive,
; and PM-DOS's own answer to this question took three releases.
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
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jnz     rgv_mount
        test    byte ptr [si+VOL_FLAGS],VF_MOUNTED
        jnz     rgv_ok
rgv_mount:
        call    rd_mount
        jc      rgv_bad
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
; rd_mount - SI = a volume record with BIOS and START filled in.  Read
;            its boot sector and work out the rest.
;=====================================================================
rd_mount:
        pusha
        push    es

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
        ; carries the old one.
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
        mov     dword ptr [si+VOL_FREEHINT],2
        or      byte ptr [si+VOL_FLAGS],VF_MOUNTED
        pop     es
        popa
        clc
        ret
rm_fail:
        and     byte ptr [si+VOL_FLAGS],NOT VF_MOUNTED
        pop     es
        popa
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
; ONE SECTOR PER CALL TO THE BIOS, deliberately.  A multi-sector
; request has to be split at a track boundary on the CHS path and at a
; 64K boundary on both, and a loop that already runs once per sector
; has neither special case in it.  What it costs is speed on a path
; that runs for a few seconds in the life of a machine.
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
        movzx   ecx,cx

rx_one:
        or      ecx,ecx
        jz      rx_ok
        mov     byte ptr [rd_retry],0

rx_try:
        test    byte ptr [rd_vflags],VF_LBA
        jz      rx_chs

        mov     word ptr [rd_dap+0],0010h
        mov     word ptr [rd_dap+2],1
        mov     word ptr [rd_dap+4],bx
        mov     word ptr [rd_dap+6],es
        mov     dword ptr [rd_dap+8],eax
        mov     dword ptr [rd_dap+12],0
        push    eax
        push    ecx
        push    si
        mov     ah,42h
        cmp     byte ptr [rd_rw],0
        je      rx_lbago
        mov     ax,4300h                ; write, no verify
rx_lbago:
        mov     dl,[rd_bios]
        mov     si,OFFSET rd_dap
        int     13h
        pop     si
        pop     ecx
        pop     eax
        jnc     rx_next
        jmp     short rx_err

rx_chs:
        push    eax
        push    ecx
        push    si
        xor     edx,edx
        movzx   ecx,word ptr [rd_spt]
        div     ecx                     ; EAX = track, EDX = sector - 1
        inc     dl
        mov     [rd_sec],dl
        xor     edx,edx
        movzx   ecx,word ptr [rd_heads]
        div     ecx                     ; EAX = cylinder, EDX = head
        mov     [rd_head],dl
        cmp     eax,1023
        ja      rx_chsbad
        mov     ch,al
        mov     cl,ah
        shl     cl,6
        or      cl,[rd_sec]
        mov     dh,[rd_head]
        mov     dl,[rd_bios]
        mov     ah,02h
        cmp     byte ptr [rd_rw],0
        je      rx_chsgo
        mov     ah,03h
rx_chsgo:
        mov     al,1
        int     13h
        pop     si
        pop     ecx
        pop     eax
        jnc     rx_next
        jmp     short rx_err
rx_chsbad:
        pop     si
        pop     ecx
        pop     eax
        jmp     short rx_dead

;--- ONE RETRY, AND IT IS A RESET.  A floppy that has not been touched
;--- for a while answers the first seek with an error and the second
;--- correctly, which is why every DOS ever written does this and why
;--- leaving it out looks exactly like a bad disk.
rx_err:
        cmp     byte ptr [rd_retry],0
        jne     rx_dead
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
        jmp     rx_try

rx_next:
        inc     eax
        dec     ecx
        add     bx,SECSIZE
        jnc     rx_one
        mov     dx,es                   ; the buffer crossed a segment, which
        add     dx,1000h                ; a 32K transfer does
        mov     es,dx
        jmp     rx_one

rx_ok:
        popad
        pop     dx
        clc
        ret
rx_dead:
        popad
        pop     dx
        stc
        ret

;=====================================================================
; 0Dh - disk reset.  Everything dirty goes out; nothing else happens.
;=====================================================================
fn0D:
        call    rd_flush
        jmp     i21_ok

;=====================================================================
; 0Eh - select a drive.  DL = it -> AL = how many letters there are.
;=====================================================================
fn0E:
        mov     al,byte ptr [bp+F_DX]
        cmp     al,LASTDRIVE
        jae     f0E_out
        mov     [rd_drive],al
f0E_out:
        mov     byte ptr [bp+F_AX],LASTDRIVE
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
        call    rd_rdsec
i25_done:
        jc      i25_bad
        pop     bp
        pop     es
        pop     ds
        xor     ax,ax
        clc
        retf                            ; the flags stay pushed - see above
i25_bad:
        pop     bp
        pop     es
        pop     ds
        mov     ax,0207h                ; sector not found / general failure
        stc
        retf

;=====================================================================
; Data
;=====================================================================
rd_vols     db  (LASTDRIVE * VOL_LEN) dup(0)
rd_bootbuf  db  SECSIZE dup(0)
rd_dap      db  16 dup(0)
rd_bios     db  0
rd_rw       db  0
rd_retry    db  0
rd_vflags   db  0
rd_spt      dw  18
rd_heads    dw  2
rd_head     db  0
rd_sec      db  0
rd_absdrv   db  0
rd_absrw    db  0
rd_abslba   dd  0
rd_abscnt   dd  0
