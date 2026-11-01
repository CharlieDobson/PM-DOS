;=====================================================================
; FLOPPY.INC - the diskette drive: what it is, what is in it, and
;              laying down a track
;
; BLOCK.INC reads and writes a diskette's sectors and has done since
; the first release.  Two things it could not do, and both showed the
; day FORMAT was asked to make a 720K disk in a 1.44M drive.
;
; IT DID NOT KNOW WHAT WAS IN THE DRIVE.  The geometry it turns a
; sector number into a cylinder, a head and a sector with came from
; INT 13h AH=08h, once, and on a real BIOS that call describes the
; DRIVE: eighteen sectors a track for a 1.44M drive whatever disk is
; in it.  An emulator answers with the image's own figures, which is
; why nothing here ever saw the difference.  A nine-sector disk read
; with eighteen-sector arithmetic is read from the wrong track from
; its second track onwards.  FDF_SETGEO is how the volume layer says
; what the boot sector of the disk in the drive claims, and fat_mount
; calls it for every diskette it mounts.
;
; AND IT COULD NOT FORMAT ONE.  A track is laid down by INT 13h AH=05h
; and the BIOS takes the gap length and the sector count for it from
; the table vector 1Eh points at, so formatting is three steps: say
; what the medium is (AH=18h, which answers with the right table),
; format and verify each track, and put the vector back.
;
; A BIOS WITHOUT AH=18h gets AH=17h where that call has a meaning and a
; table of our own in GV_FDPT, copied from the BIOS's and given the
; three bytes a format reads.
;
; AND IT COULD NOT READ PAST THE TABLE'S LAST SECTOR.  The same table
; says where a track ends for a read or a write, and a DMF disk's
; tracks are longer than a 1.44M drive's table says.  fpy_rwup and
; fpy_rwdown lengthen it for the length of one block-layer call.
;
; AND IT COULD NOT SAY WHEN THE DISK HAD BEEN CHANGED.  FDF_CHANGED is
; the volume layer's media check (DOS\FAT.INC, flp_chkmedia), answered
; from the BIOS's 06h kept by bio_retry, the time of the last transfer,
; the change line, and the boot sector FDF_MOUNTED kept.
;
; Included by IOPM.ASM, after BLOCK.INC.
;=====================================================================

IVT_INT1E       EQU     1Eh * 4

; Until PMDOS.INC has these beside the other FDF_* numbers.  DOS\FAT.INC
; carries the same lines, and the guard keeps both right once PMDOS.INC
; does.
IFNDEF FDF_CHANGED
FDF_CHANGED     EQU     6       ; EDI -> the FAT's first sector or 0 ->
                                ; AH = 0 the same disk, 1 another one
FDF_MOUNTED     EQU     7       ; ESI -> the boot sector just mounted
ENDIF
FPY_SAME_TICKS  EQU     36      ; two seconds of the BIOS's 18.2 a second
FPY_IDLEN       EQU     64      ; the boot sector bytes a disk is known by

FPY_NTRIES       EQU     3
FPY_DMFGAP       EQU     0Ch            ; DMF's format gap

; the diskette parameter table, as far as a format reads it
DPT_SIZE        EQU     3               ; 2 = 512 bytes a sector
DPT_EOT         EQU     4               ; sectors a track
DPT_FMTGAP      EQU     7
DPT_FILL        EQU     8
DPT_LEN         EQU     11

;=====================================================================
; fpy_svc - IOS_FLOPPY.  AL = drive, AH = FDF_*.
;=====================================================================
fpy_svc:
        cmp     al,2
        jae     fpy_nodrive
        mov     byte ptr [fpy_drive],al
        cmp     ah,FDF_TYPE
        je      fpy_gettype
        cmp     ah,FDF_SETGEO
        je      fpy_setgeo
        cmp     ah,FDF_BEGIN
        je      fpy_begin
        cmp     ah,FDF_TRACK
        je      fpy_track
        cmp     ah,FDF_END
        je      fpy_end
        cmp     ah,FDF_FORGET
        je      fpy_forget
        cmp     ah,FDF_CHANGED
        je      fpy_changed
        cmp     ah,FDF_MOUNTED
        je      fpy_mounted
fpy_nodrive:
        mov     ah,01h                  ; the BIOS's "bad command"
        stc
        ret

;=====================================================================
; FDF_TYPE - what kind of drive is it?
;            -> BL = FDT_*, CL = sectors a track, CH = cylinders,
;               DH = heads.  CF if there is no such drive.
;
; The BIOS is asked first and CMOS second.  AH=08h on an AT answers
; the type in BL; a BIOS that leaves BL alone, or a drive number it
; does not know, is what register 10h is for - the byte SETUP wrote,
; drive 0 in its high half.
;=====================================================================
fpy_gettype:
        push    eax
        push    esi
        call    fpy_asktype
        movzx   esi,byte ptr [fpy_kind]
        or      esi,esi
        jz      fpy_tnone
        mov     bl,byte ptr [fpy_kind]
        mov     cl,byte ptr [fpy_kindspt+esi]
        mov     ch,byte ptr [fpy_kindcyl+esi]
        mov     dh,2
        pop     esi
        pop     eax
        clc
        ret
fpy_tnone:
        pop     esi
        pop     eax
        mov     ah,01h
        stc
        ret

;---------------------------------------------------------------------
; fpy_asktype - fpy_kind = FDT_* for fpy_drive, FDT_NONE if there is
;               no such drive.  All registers kept.
;---------------------------------------------------------------------
fpy_asktype:
        push    eax
        mov     byte ptr [fpy_kind],FDT_NONE
        call    rm_clear
        mov     byte ptr ds:[RMR_EAX+1],08h
        mov     al,byte ptr [fpy_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     al,13h
        call    rm_int
        call    rm_cf
        jnz     fpy_atcmos
        mov     al,byte ptr [fpy_drive] ; DL = how many there are
        cmp     al,byte ptr ds:[RMR_EDX]
        jae     fpy_atcmos
        mov     al,byte ptr ds:[RMR_EBX]
        call    fpy_kindok
        jnc     fpy_athave
fpy_atcmos:
        mov     al,10h
        out     70h,al
        call    io_delay
        in      al,71h
        cmp     byte ptr [fpy_drive],0
        jne     fpy_atlow
        shr     al,4
fpy_atlow:
        and     al,0Fh
        call    fpy_kindok
        jc      fpy_atout
fpy_athave:
        mov     byte ptr [fpy_kind],al
fpy_atout:
        pop     eax
        ret

; AL = a type byte -> CF clear and AL = FDT_* if it is one.  Some
; BIOSes call a 2.88M drive 6.
fpy_kindok:
        cmp     al,6
        jne     fpy_kinrange
        mov     al,FDT_2880
fpy_kinrange:
        cmp     al,FDT_360
        jb      fpy_kbad
        cmp     al,FDT_2880
        ja      fpy_kbad
        clc
        ret
fpy_kbad:
        stc
        ret

;=====================================================================
; FDF_SETGEO - CX = sectors a track, DX = heads, EBX = sectors in all.
;
; Checked, because the figures come off a disk and a disk can say
; anything: a track of more than 36 sectors or a third side is not a
; diskette, and the geometry the BIOS gave is left standing.
;=====================================================================
fpy_setgeo:
        push    eax
        push    ebx
        push    esi
        or      cx,cx
        jz      fpy_sgbad
        cmp     cx,36
        ja      fpy_sgbad
        or      dx,dx
        jz      fpy_sgbad
        cmp     dx,2
        ja      fpy_sgbad
        or      ebx,ebx
        jz      fpy_sgbad
        cmp     ebx,5760 + 720          ; 2.88M, and a few tracks over
        ja      fpy_sgbad
        mov     esi,ebx
        mov     al,byte ptr [fpy_drive]
        call    blk_slot
        jc      fpy_sgbad
        mov     word ptr [ebx+GEO_SPT],cx
        mov     word ptr [ebx+GEO_HEADS],dx
        mov     dword ptr [ebx+GEO_TOTAL],esi
        mov     word ptr [ebx+GEO_EXT],0
        mov     dword ptr [ebx+GEO_LBATOT],0
        mov     word ptr [ebx+GEO_FLAGS],1
        pop     esi
        pop     ebx
        pop     eax
        clc
        ret
fpy_sgbad:
        pop     esi
        pop     ebx
        pop     eax
        mov     ah,01h
        stc
        ret

;=====================================================================
; FDF_FORGET - the geometry is the BIOS's to say again.
;=====================================================================
fpy_forget:
        push    ebx
        mov     al,byte ptr [fpy_drive]
        call    blk_slot
        jc      fpy_fgout
        mov     word ptr [ebx+GEO_FLAGS],0
fpy_fgout:
        pop     ebx
        clc
        ret

;=====================================================================
; FDF_CHANGED - is the disk in the drive still the one FDF_MOUNTED was
;               shown?  EDI -> 512 bytes the first FAT sector (sector
;               1) should hold, or 0 not to compare it.  -> AH = 0 the
;               same disk, 1 another one or none that reads.  CF clear.
;               Everything else kept.
;
; THREE WITNESSES FIRST, cheapest first, and the first two cost no BIOS
; call.  The latch: the BIOS said 06h to a read (bio_retry), the one
; certain notice there is.  The clock: a drive that answered within the
; last two seconds holds the disk it held then, which is MS-DOS's rule
; for a drive with no change line, and nobody has ever swapped one
; faster.  The change line: INT 13h AH=16h, on a BIOS whose AH=15h says
; the drive has one (type 2), asked once a drive and kept.
;
; WHERE ANY OF THEM SAYS "PERHAPS", THE DISK IS READ, and "changed" is
; also only "perhaps": a door opened and shut on the same disk raises
; the line as well, and that disk is still the one whose directory the
; user is in.  Sector 0, and sector 1 with it when the caller has a FAT
; to compare, in one transfer into the staging buffer, and the first
; FPY_IDLEN bytes of the boot sector - the BPB, the serial at 27h and
; the label after it - against the ones kept.  The latch is cleared
; after the read, because a 06h that read met is this change and not
; another one.
;=====================================================================
fpy_changed:
        pushad
        movzx   ecx,byte ptr [fpy_drive]
        xor     dl,dl
        xchg    dl,byte ptr [fpy_chg+ecx]       ; read and cleared
        or      dl,dl
        jnz     fch_look
        mov     edx,dword ptr ds:[BDA_TICKS]
        sub     edx,dword ptr [fpy_last+ecx*4]
        cmp     edx,FPY_SAME_TICKS      ; past midnight it comes out huge,
        jbe     fch_same                ; which is one look a day

        mov     dl,byte ptr [fpy_line+ecx]
        or      dl,dl
        jnz     fch_known
        call    rm_clear                ; AH=15h: does it have a line?
        mov     byte ptr ds:[RMR_EAX+1],15h
        mov     byte ptr ds:[RMR_EDX],cl
        mov     al,13h
        call    rm_int
        mov     dl,1                    ; no, until the BIOS says type 2
        call    rm_cf
        jnz     fch_keep
        cmp     byte ptr ds:[RMR_EAX+1],2
        jne     fch_keep
        mov     dl,2
fch_keep:
        mov     byte ptr [fpy_line+ecx],dl
IFDEF FDD_DEBUG
        mov     esi,OFFSET msgLine
        call    con_str
        movzx   eax,dl
        call    con_hex8
        mov     al,' '
        call    con_out
ENDIF
fch_known:
        cmp     dl,2
        jne     fch_look
        call    rm_clear                ; AH=16h: has it changed?
        mov     byte ptr ds:[RMR_EAX+1],16h
        mov     byte ptr ds:[RMR_EDX],cl
        mov     al,13h
        call    rm_int
IFDEF FDD_DEBUG
        mov     esi,OFFSET msgChg
        call    con_str
        movzx   eax,byte ptr ds:[RMR_EAX+1]
        call    con_hex8
        mov     al,' '
        call    con_out
ENDIF
        call    rm_cf
        jnz     fch_look                ; 06h, or a status that says nothing
        cmp     byte ptr ds:[RMR_EAX+1],0
        je      fch_same

fch_look:
        mov     ebp,edi                 ; EBP = the caller's FAT sector, or 0
        mov     al,cl
        xor     ebx,ebx
        mov     cx,1
        or      ebp,ebp
        jz      fch_rd
        inc     ecx
fch_rd:
        mov     edi,XFER_BUF            ; the staging buffer, lifted onto
        call    blk_read                ; itself: nothing else is in it
        movzx   ecx,byte ptr [fpy_drive]
        mov     byte ptr [fpy_chg+ecx],0
        jc      fch_new                 ; no disk, or none that reads
        mov     esi,ecx
        shl     esi,6                   ; * FPY_IDLEN
        add     esi,OFFSET fpy_id
        mov     edi,XFER_BUF
        mov     ecx,FPY_IDLEN / 4
        cld
        repe    cmpsd
        jne     fch_new
        mov     esi,ebp
        or      esi,esi
        jz      fch_same
        mov     edi,XFER_BUF + 512
        mov     ecx,512 / 4
        repe    cmpsd
        jne     fch_new
fch_same:
        popad
        xor     ah,ah
        clc
        ret
fch_new:
IFDEF FDD_DEBUG
        mov     esi,OFFSET msgNew
        call    con_str
ENDIF
        popad
        mov     ah,1
        clc
        ret

;=====================================================================
; FDF_MOUNTED - ESI -> the boot sector the volume layer has just read
;               and is mounting.  Its first FPY_IDLEN bytes are what
;               FDF_CHANGED knows the disk by, and the latch is cleared:
;               a 06h on that read was the change it is mounting for.
;=====================================================================
fpy_mounted:
        pushad
        movzx   edi,byte ptr [fpy_drive]
        mov     byte ptr [fpy_chg+edi],0
        shl     edi,6                   ; * FPY_IDLEN
        add     edi,OFFSET fpy_id
        mov     ecx,FPY_IDLEN / 4
        cld
        rep     movsd
        popad
        clc
        ret
IF FPY_IDLEN - 64
        .ERR    fpy_changed and fpy_mounted multiply by FPY_IDLEN with SHL 6
ENDIF

;=====================================================================
; FDF_BEGIN - CL = sectors a track, CH = cylinders, BL = FDM_*.
;
; Vector 1Eh is kept the first time and put back by FDF_END, so a
; program that begins twice - a second disk - does not lose the
; BIOS's own table behind its own.
;
; DMF IS A 1.44M DISK WITH 21 SECTORS A TRACK, so it is asked for as
; FDM_1440 with more than 18 in CL.  No BIOS knows it: AH=18h is asked
; for the 1.44M shape, which sets the data rate, and the table it
; answers with is copied and given 21 sectors and DMF's format gap of
; 0Ch - the figure Linux's floppy driver formats its 1.68M type with.
; Twelve bytes between sectors is too little time for a program that
; reads one sector a call to catch the next, so FDF_TRACK lays these
; tracks down two to one (1, 12, 2, 13, ...), as Microsoft's own DMF
; disks are and as Linux does for any gap that small.
;=====================================================================
fpy_begin:
        pushad
        mov     byte ptr [fpy_spt],cl
        mov     byte ptr [fpy_cyls],ch
        mov     byte ptr [fpy_medium],bl
        mov     byte ptr [fpy_dmf],0
        cmp     bl,FDM_1440
        jne     fpy_bshape
        cmp     cl,18
        jbe     fpy_bshape
        mov     byte ptr [fpy_dmf],1
fpy_bshape:

        cmp     byte ptr [fpy_open],0
        jne     fpy_bkept
        mov     eax,dword ptr ds:[IVT_INT1E]
        mov     dword ptr [fpy_old1e],eax
        mov     byte ptr [fpy_open],1
fpy_bkept:
        mov     eax,dword ptr [fpy_old1e]       ; every attempt starts from
        mov     dword ptr ds:[IVT_INT1E],eax    ; the BIOS's table

        call    fpy_reset

        call    rm_clear
        mov     byte ptr ds:[RMR_EAX+1],18h
        mov     al,byte ptr [fpy_cyls]
        dec     al
        mov     byte ptr ds:[RMR_ECX+1],al
        mov     al,byte ptr [fpy_spt]
        cmp     byte ptr [fpy_dmf],0
        je      fpy_b18spt
        mov     al,18                   ; DMF: the BIOS knows 1.44M
fpy_b18spt:
        mov     byte ptr ds:[RMR_ECX],al
        mov     al,byte ptr [fpy_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     al,13h
        call    rm_int
        call    rm_cf
        jnz     fpy_bno18

        movzx   eax,word ptr ds:[RMR_ES]
        shl     eax,16
        mov     ax,word ptr ds:[RMR_EDI]
        or      eax,eax
        jz      fpy_bown                ; it said yes and gave no table
        cmp     byte ptr [fpy_dmf],0
        jne     fpy_bfrom               ; that table, lengthened
        mov     dword ptr ds:[IVT_INT1E],eax
        jmp     fpy_bok

fpy_bno18:
        mov     al,byte ptr ds:[RMR_EAX+1]
        mov     byte ptr [fpy_status],al
        cmp     al,0Ch                  ; not a medium this drive takes
        je      fpy_bfail
        cmp     al,80h                  ; nothing in the drive
        je      fpy_bfail

        ; A BIOS WITHOUT THE CALL.  AH=17h is the one before it and
        ; knows four combinations, none of them 1.44M
        call    fpy_asktype
        xor     ah,ah
        mov     al,byte ptr [fpy_medium]
        cmp     al,FDM_360
        jne     fpy_b17b
        mov     ah,1
        cmp     byte ptr [fpy_kind],FDT_360
        je      fpy_b17go
        mov     ah,2
        jmp     short fpy_b17go
fpy_b17b:
        cmp     al,FDM_1200
        jne     fpy_b17c
        mov     ah,3
        jmp     short fpy_b17go
fpy_b17c:
        cmp     al,FDM_720
        jne     fpy_bown
        mov     ah,4
fpy_b17go:
        push    eax
        call    rm_clear
        pop     eax
        mov     byte ptr ds:[RMR_EAX],ah
        mov     byte ptr ds:[RMR_EAX+1],17h
        mov     al,byte ptr [fpy_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     al,13h
        call    rm_int                  ; its answer is not waited on: a
                                        ; BIOS with neither call formats
                                        ; what the drive is, and that is
                                        ; still worth trying

fpy_bown:
        ; the BIOS's table, with the three bytes a format reads
        mov     eax,dword ptr [fpy_old1e]
fpy_bfrom:
        ; ...or the one in EAX
        movzx   esi,ax
        shr     eax,16
        shl     eax,4
        add     esi,eax
        mov     edi,GV_FDPT
        mov     ecx,DPT_LEN
        cld
        rep     movsb
        mov     byte ptr ds:[GV_FDPT+DPT_SIZE],2
        mov     al,byte ptr [fpy_spt]
        mov     byte ptr ds:[GV_FDPT+DPT_EOT],al
        movzx   esi,byte ptr [fpy_medium]
        mov     al,byte ptr [fpy_gaps+esi]
        cmp     byte ptr [fpy_dmf],0
        je      fpy_bgap
        mov     al,FPY_DMFGAP
fpy_bgap:
        mov     byte ptr ds:[GV_FDPT+DPT_FMTGAP],al
        mov     byte ptr ds:[GV_FDPT+DPT_FILL],0F6h
        mov     dword ptr ds:[IVT_INT1E],(IO_SEG SHL 16) OR GVO_FDPT

fpy_bok:
        popad
        xor     ah,ah
        clc
        ret
fpy_bfail:
        popad
        mov     ah,byte ptr [fpy_status]
        stc
        ret

;=====================================================================
; FDF_TRACK - CH = cylinder, DH = head, CL = sectors a track.  The
;             track is formatted and then read back; CF and AH = the
;             BIOS's status if either could not be done.
;
; A WRITE-PROTECTED DISK IS NOT TRIED AGAIN.  Every other failure is,
; after a reset, because the first command after a disk has been
; changed fails by design and a marginal track deserves a second go.
;=====================================================================
fpy_track:
        pushad
        mov     byte ptr [fpy_spt],cl
        mov     byte ptr [fpy_cyl],ch
        mov     byte ptr [fpy_head],dh

        mov     byte ptr [fpy_tries],FPY_NTRIES
fpy_tfmt:
        ; the address fields: cylinder, head, sector, size, in the
        ; order they go round the track.  One to one, or for DMF two
        ; to one: the even places take 1, 2, 3 ... and the odd ones
        ; what is left, so 21 sectors go 1, 12, 2, 13 ... 21, 11.
        mov     edi,XFER_BUF
        movzx   ecx,byte ptr [fpy_spt]
        xor     ebx,ebx                 ; EBX = the place on the track
fpy_tfield:
        mov     al,byte ptr [fpy_cyl]
        mov     byte ptr [edi],al
        mov     al,byte ptr [fpy_head]
        mov     byte ptr [edi+1],al
        mov     al,bl
        inc     al
        cmp     byte ptr [fpy_dmf],0
        je      fpy_tsec
        mov     al,bl
        shr     al,1
        inc     al
        test    bl,1
        jz      fpy_tsec
        mov     al,byte ptr [fpy_spt]
        inc     al
        shr     al,1
        mov     ah,bl
        inc     ah
        shr     ah,1
        add     al,ah
fpy_tsec:
        mov     byte ptr [edi+2],al
        mov     byte ptr [edi+3],2
        add     edi,4
        inc     ebx
        cmp     ebx,ecx
        jb      fpy_tfield

        mov     ah,05h
        call    fpy_chs
        jz      fpy_tverify
        call    fpy_tagain
        jnc     fpy_tfmt
        jmp     short fpy_tfail

fpy_tverify:
        mov     byte ptr [fpy_tries],FPY_NTRIES
fpy_tvfy:
        mov     ah,04h
        call    fpy_chs
        jz      fpy_tok
        call    fpy_tagain
        jnc     fpy_tvfy
fpy_tfail:
        popad
        mov     ah,byte ptr [fpy_status]
        stc
        ret
fpy_tok:
        popad
        xor     ah,ah
        clc
        ret

;---------------------------------------------------------------------
; fpy_chs - INT 13h AH on the whole of the track in fpy_cyl and
;           fpy_head.  ZF set if the BIOS said yes; fpy_status has its
;           answer either way.
;---------------------------------------------------------------------
fpy_chs:
        push    eax
        call    rm_clear
        pop     eax
        mov     byte ptr ds:[RMR_EAX+1],ah
        mov     al,byte ptr [fpy_spt]
        mov     byte ptr ds:[RMR_EAX],al
        mov     al,byte ptr [fpy_cyl]
        mov     byte ptr ds:[RMR_ECX+1],al
        mov     byte ptr ds:[RMR_ECX],1
        mov     al,byte ptr [fpy_head]
        mov     byte ptr ds:[RMR_EDX+1],al
        mov     al,byte ptr [fpy_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     word ptr ds:[RMR_ES],XFER_SEG
        mov     dword ptr ds:[RMR_EBX],0
        mov     al,13h
        call    rm_int
        mov     al,byte ptr ds:[RMR_EAX+1]
        mov     byte ptr [fpy_status],al
        call    rm_cf
        ret

;---------------------------------------------------------------------
; fpy_tagain - a command failed: CF clear if it is worth another go,
;              and the controller has been reset for it.
;---------------------------------------------------------------------
fpy_tagain:
        cmp     byte ptr [fpy_status],03h       ; write protected
        je      fpy_tano
        dec     byte ptr [fpy_tries]
        jz      fpy_tano
        push    eax
        mov     al,byte ptr [fpy_status]        ; the reset has a status of
        push    eax                             ; its own and this one is
        call    fpy_reset                       ; the one to report
        pop     eax
        mov     byte ptr [fpy_status],al
        pop     eax
        clc
        ret
fpy_tano:
        stc
        ret

fpy_reset:
        push    eax
        call    rm_clear
        mov     al,byte ptr [fpy_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     al,13h
        call    rm_int
        pop     eax
        ret

;=====================================================================
; FDF_END - the vector goes back, and the controller is reset so that
;           the next read starts from the BIOS's own table.
;=====================================================================
fpy_end:
        push    eax
        cmp     byte ptr [fpy_open],0
        je      fpy_eout
        mov     eax,dword ptr [fpy_old1e]
        mov     dword ptr ds:[IVT_INT1E],eax
        mov     byte ptr [fpy_open],0
        call    fpy_reset
fpy_eout:
        pop     eax
        clc
        ret

;=====================================================================
; fpy_rwup - EBX = the geometry cache entry of a diskette about to be
;            read or written.  If the table vector 1Eh points at ends a
;            track before the disk does, the vector is pointed at a copy
;            that does not.  All registers and flags kept.
;
; THE BIOS ENDS A TRANSFER AT THE TABLE'S LAST SECTOR, NOT THE DISK'S.
; INT 13h hands the controller the sector count from the table, and a
; controller that reaches that sector before the transfer is finished
; stops and reports the end of the cylinder.  A 1.44M drive's table
; says eighteen.  A DMF disk has twenty-one sectors a track, so sectors
; 17 to 21 in one call failed at 18, while a call that began at 19 went
; through: some files read and others did not.  Emulators never showed
; it, because their INT 13h does not read the table.
;
; A table that says more than the disk has costs nothing, because the
; transfer ends when the DMA count runs out: that is how a 720K disk is
; read through a 1.44M drive's table.  MS-DOS raises the same byte round
; every call and puts it back afterwards, but only ever as far as the
; longest track of a drive it knows, never as far as the disk's.
;
; A format in progress is left alone: FDF_BEGIN has pointed the vector
; at a table made for that disk.
;=====================================================================
fpy_rwup:
        pushfd
        pushad
        cmp     byte ptr [fpy_rwon],0
        jne     fru_out
        cmp     byte ptr [fpy_open],0
        jne     fru_out
        mov     eax,dword ptr ds:[IVT_INT1E]
        movzx   esi,ax
        shr     eax,16
        shl     eax,4
        add     esi,eax                 ; ESI = the table, flat
        movzx   eax,byte ptr [esi+DPT_EOT]
        cmp     ax,word ptr [ebx+GEO_SPT]
        jae     fru_out                 ; it reaches the end of the track

        mov     eax,dword ptr ds:[IVT_INT1E]
        mov     dword ptr [fpy_rwold],eax
        mov     edi,GV_RWDPT
        mov     ecx,DPT_LEN
        cld
        rep     movsb
        mov     al,byte ptr [ebx+GEO_SPT]
        mov     byte ptr ds:[GV_RWDPT+DPT_EOT],al
        mov     dword ptr ds:[IVT_INT1E],(IO_SEG SHL 16) OR GVO_RWDPT
        mov     byte ptr [fpy_rwon],1
fru_out:
        popad
        popfd
        ret

;=====================================================================
; fpy_rwdown - vector 1Eh back where fpy_rwup found it.  All registers
;              and flags kept.
;=====================================================================
fpy_rwdown:
        pushfd
        cmp     byte ptr [fpy_rwon],0
        je      frd_out
        push    eax
        mov     eax,dword ptr [fpy_rwold]
        mov     dword ptr ds:[IVT_INT1E],eax
        mov     byte ptr [fpy_rwon],0
        pop     eax
frd_out:
        popfd
        ret

;=====================================================================
fpy_rwold   dd  0                       ; vector 1Eh before fpy_rwup
fpy_rwon    db  0                       ; and it has been moved
            db  0,0,0
fpy_old1e   dd  0                       ; vector 1Eh as the BIOS had it
fpy_drive   db  0
fpy_open    db  0                       ; a format is in progress
fpy_kind    db  0                       ; FDT_*
fpy_medium  db  0                       ; FDM_*
fpy_spt     db  0
fpy_cyls    db  0
fpy_cyl     db  0
fpy_head    db  0
fpy_tries   db  0
fpy_status  db  0
fpy_dmf     db  0                       ; FDF_BEGIN was asked for DMF
            db  0
fpy_last    dd  0, 0                    ; the tick of each drive's last good
                                        ; transfer (bio_done)
fpy_chg     db  0, 0                    ; the BIOS said 06h (bio_retry)
fpy_line    db  0, 0                    ; AH=15h's answer: 0 not asked yet,
                                        ; 1 no change line, 2 one
            db  0, 0
fpy_id      db  2 * FPY_IDLEN dup(0)    ; each drive's boot sector, as
                                        ; FDF_MOUNTED was shown it
IFDEF FDD_DEBUG
msgLine     db  '[15 line=',0
msgChg      db  '[16 ah=',0
msgNew      db  '[new disk] ',0
ENDIF

; by FDT_*: what the drive holds when it is full
fpy_kindspt db  0, 9, 15, 9, 18, 36
fpy_kindcyl db  0, 40, 80, 80, 80, 80

; by FDM_*: the gap a format leaves between sectors
fpy_gaps    db  0, 50h, 54h, 50h, 6Ch, 53h
