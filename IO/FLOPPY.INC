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
; Included by IOPM.ASM, after BLOCK.INC.
;=====================================================================

IVT_INT1E       EQU     1Eh * 4

FPY_NTRIES       EQU     3

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
; FDF_BEGIN - CL = sectors a track, CH = cylinders, BL = FDM_*.
;
; Vector 1Eh is kept the first time and put back by FDF_END, so a
; program that begins twice - a second disk - does not lose the
; BIOS's own table behind its own.
;=====================================================================
fpy_begin:
        pushad
        mov     byte ptr [fpy_spt],cl
        mov     byte ptr [fpy_cyls],ch
        mov     byte ptr [fpy_medium],bl

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
        ; the address fields: cylinder, head, sector, size
        mov     edi,XFER_BUF
        movzx   ecx,byte ptr [fpy_spt]
        mov     bl,1
fpy_tfield:
        mov     al,byte ptr [fpy_cyl]
        mov     byte ptr [edi],al
        mov     al,byte ptr [fpy_head]
        mov     byte ptr [edi+1],al
        mov     byte ptr [edi+2],bl
        mov     byte ptr [edi+3],2
        add     edi,4
        inc     bl
        dec     ecx
        jnz     fpy_tfield

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
            db  0,0

; by FDT_*: what the drive holds when it is full
fpy_kindspt db  0, 9, 15, 9, 18, 36
fpy_kindcyl db  0, 40, 80, 80, 80, 80

; by FDM_*: the gap a format leaves between sectors
fpy_gaps    db  0, 50h, 54h, 50h, 6Ch, 53h
