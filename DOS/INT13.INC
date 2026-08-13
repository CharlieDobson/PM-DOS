;=====================================================================
; INT13.INC - the disk BIOS, as a DOS box sees it
;
; Until now a guest's INT 13h reflected straight to the real BIOS, and
; that was a hole under everything above it.  The kernel owns the disks
; - it talks to IDE controllers itself, it caches FAT sectors, whole
; FAT12 FATs and directory sectors, and it holds writes until something
; asks for them - and a program going round all of that to the BIOS
; could:
;
;   read a sector the kernel was holding a newer version of, and act on
;   the old contents;
;
;   write a sector the kernel had cached, after which the cache was
;   wrong and stayed wrong until the volume was unmounted;
;
;   write a sector the kernel later overwrote from a cache of its own,
;   silently undoing the program's work;
;
;   and on an ATA disk, drive the hardware through a second, entirely
;   independent path - the BIOS's - while the kernel's own PIO
;   sequences were half-finished.
;
; So INT 13h is served here, out of the same block layer and the same
; volume table as everything else, and the caches are made to agree
; with the medium around every transfer (see vol_syncdrv in FAT.INC).
;
; DRIVES THE KERNEL DOES NOT KNOW are reflected to the BIOS untouched.
; The block layer has slots for two floppies and two hard disks; a
; machine with a third disk has one this kernel never scanned, holding
; no volume of ours and no cache to be wrong, and the honest answer for
; it is the BIOS's.
;
; Included by DOSMAIN.ASM after VDM.INC - it works on vdm_ctx.
;=====================================================================

;=====================================================================
; vdm_i13 - INT 13h from the guest.  Called with the box's context in
;           vdm_ctx; returns to vdm_loop by way of the dispatcher.
;=====================================================================
vdm_i13:
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX+1]       ; AH = function
        mov     [i13_fn],al
IFDEF I13_DEBUG
        push    eax
        mov     esi,OFFSET msgI13
        call    dword ptr ds:[IOS_CONSTR]
        movzx   eax,byte ptr [i13_fn]
        call    vdm_hex8
        mov     al,'/'
        call    dword ptr ds:[IOS_CONOUT]
        movzx   eax,byte ptr [vdm_ctx+V86C_EDX]
        call    vdm_hex8
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        pop     eax
ENDIF
        mov     al,byte ptr [vdm_ctx+V86C_EDX]          ; DL = drive
        mov     [i13_drive],al

        ; A drive we have no geometry for is a drive we do not manage.
        call    dword ptr ds:[IOS_DRVGEO]
        jc      i13_reflect
        mov     [i13_spt],ecx
        mov     [i13_heads],edx
        mov     [i13_chstotal],ebx

        mov     al,[i13_fn]
        cmp     al,00h
        je      i13_reset
        cmp     al,01h
        je      i13_status
        cmp     al,02h
        je      i13_read
        cmp     al,03h
        je      i13_write
        cmp     al,04h
        je      i13_verify
        cmp     al,08h
        je      i13_params
        cmp     al,15h
        je      i13_type
        cmp     al,41h
        je      i13_eddchk
        cmp     al,42h
        je      i13_extread
        cmp     al,43h
        je      i13_extwrite
        cmp     al,48h
        je      i13_extparams

;---------------------------------------------------------------------
; Everything else - AH=05h format a track above all - goes to the real
; BIOS, because refusing it would mean a DOS box could never format a
; floppy and the block layer has no way to do it for them.
;
; Both halves of the cache reconciliation happen BEFORE the reflection,
; and they have to: once the interrupt is pushed into the guest's IVT
; this handler is finished, and control does not come back here when
; the BIOS returns.  Discarding first is still correct - the caches are
; emptied, the BIOS then writes, and the next thing to want a sector
; reads it fresh off the medium.
;
; Over the whole disk, since we do not know what an unrecognised
; function is about to touch.  A volume whose boot sector is inside
; that range comes back unmounted, which for a track format is exactly
; right and for a "test drive ready" is merely expensive - and one of
; those two happens.
;---------------------------------------------------------------------
i13_reflect:
        mov     al,[i13_drive]
        mov     ah,0
        xor     ebx,ebx
        mov     ecx,0FFFFFFFFh
        call    vol_syncdrv                     ; flush what is dirty
        mov     al,[i13_drive]
        mov     ah,1
        xor     ebx,ebx
        mov     ecx,0FFFFFFFFh
        call    vol_syncdrv                     ; then forget all of it
        mov     ebx,13h
        jmp     vdm_reflect

;=====================================================================
; AH=00h - reset the disk system.
;
; Nothing to do and nothing to undo: the retries the real function is
; there to recover from are the block layer's business, and it does
; them per transfer.  Answering "done" is both true and what the caller
; is waiting to hear.
;=====================================================================
i13_reset:
        xor     al,al
        jmp     i13_ok

;=====================================================================
; AH=01h - the status of the last operation, out of the same BIOS data
;          area byte the real one uses.  A guest that reads 0040:0041
;          directly - and some do - sees the same answer.
;=====================================================================
i13_status:
        mov     al,byte ptr ds:[BDA_DSKSTAT]
        xor     ah,ah
        mov     word ptr [vdm_ctx+V86C_EAX],ax  ; AL = status, AH = 0
        jmp     i13_clc

;=====================================================================
; AH=02h / 03h / 04h - read, write and verify, addressed by CHS.
;=====================================================================
i13_read:
        mov     byte ptr [i13_iswrite],0
        jmp     short i13_rw
i13_write:
        mov     byte ptr [i13_iswrite],1
        jmp     short i13_rw
i13_verify:
        mov     byte ptr [i13_iswrite],2

i13_rw:
        movzx   ecx,byte ptr [vdm_ctx+V86C_EAX] ; AL = sector count
        or      ecx,ecx
        jz      i13_badcmd                      ; nought sectors is not a
        mov     [i13_count],ecx                 ; request, it is a mistake

        call    i13_chs2lba
        jc      i13_badsec

        ; ES:BX, and a verify does not have one
        cmp     byte ptr [i13_iswrite],2
        je      i13_rwgo
        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        shl     eax,4
        movzx   edx,word ptr [vdm_ctx+V86C_EBX]
        add     eax,edx
        mov     [i13_buf],eax
        call    i13_bufok
        jc      i13_badbuf

i13_rwgo:
IFDEF I13_DEBUG
        mov     esi,OFFSET msgI13L
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,[i13_lba]
        call    vdm_hex32
        mov     esi,OFFSET msgI13B
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,[i13_buf]
        call    vdm_hex32
        mov     esi,OFFSET msgI13N
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,[i13_count]
        call    vdm_hex8
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
ENDIF
        call    i13_flush                       ; our dirt, out first
        cmp     byte ptr [i13_iswrite],2
        je      i13_doverify

        mov     al,[i13_drive]
        mov     ebx,[i13_lba]
        mov     ecx,[i13_count]
        mov     edi,[i13_buf]
        cmp     byte ptr [i13_iswrite],0
        je      i13_dord
        call    dword ptr ds:[IOS_WRBLK]
        jc      i13_ioerr
        call    i13_forget                      ; their sectors now
        jmp     short i13_moved
i13_dord:
        call    dword ptr ds:[IOS_RDBLK]
        jc      i13_ioerr
i13_moved:
        mov     eax,[i13_count]                 ; AL = sectors transferred
        jmp     i13_ok

;--- a verify moves nothing to the caller, so it reads a sector at a
;--- time into a buffer of our own and throws every one of them away.
;--- The point of the call is whether the medium can be read at all.
i13_doverify:
        mov     ecx,[i13_count]
        mov     ebx,[i13_lba]
i13_vone:
        push    ecx
        push    ebx
        mov     al,[i13_drive]
        mov     ecx,1
        mov     edi,OFFSET i13_scratch
        call    dword ptr ds:[IOS_RDBLK]
        pop     ebx
        pop     ecx
        jc      i13_ioerr
        inc     ebx
        dec     ecx
        jnz     i13_vone
        mov     eax,[i13_count]
        jmp     i13_ok

;=====================================================================
; AH=08h - the drive's parameters, in the packed form of 1983.
;
; Straight out of the geometry the block layer cached from the BIOS at
; boot, so a guest's arithmetic and the kernel's arrive at the same
; sector for the same CHS triple.  Handing back the disk's TRUE size
; here would be worse than useless: above 1024 cylinders there is
; nowhere in CX to put the number.
;=====================================================================
i13_params:
        mov     eax,[i13_chstotal]              ; cylinders = total / (H*S)
        xor     edx,edx
        mov     ecx,[i13_heads]
        imul    ecx,[i13_spt]
        or      ecx,ecx                         ; JECXZ only reaches 127 bytes
        jz      i13_badcmd
        div     ecx
        or      eax,eax
        jz      i13_badcmd
        cmp     eax,1024                        ; CX holds ten bits of them
        jbe     i13_pcyl
        mov     eax,1024
i13_pcyl:
        dec     eax                             ; the LAST cylinder

        mov     ecx,eax
        shr     ecx,2
        and     cl,0C0h                         ; CL 7:6 = cylinder bits 9:8
        or      ecx,[i13_spt]                   ; CL 5:0 = sectors per track
        and     ecx,0FFh
        shl     eax,8                           ; CH = cylinder bits 7:0
        and     eax,0FF00h
        or      ecx,eax
        mov     word ptr [vdm_ctx+V86C_ECX],cx

        mov     edx,[i13_heads]
        dec     edx                             ; DH = the LAST head
        shl     edx,8
        mov     dl,[i13_hdcount]                ; DL = drives LIKE THIS ONE,
        cmp     byte ptr [i13_drive],80h        ; which is two counts and not
        jae     i13_pdl                         ; one: a program asking about
        mov     dl,[i13_fdcount]                ; A: is told how many floppies
i13_pdl:
        mov     word ptr [vdm_ctx+V86C_EDX],dx

        ; BL is the drive type, which needs a media sense this kernel
        ; does not do, so it is zeroed rather than invented.
        mov     word ptr [vdm_ctx+V86C_EBX],0

        ; ES:DI IS LEFT ALONE, deliberately.  The call is documented as
        ; returning the floppy parameter table there, and a real BIOS
        ; duly destroys the caller's ES to do it - but we have no such
        ; table to point at, so zeroing ES would cost the caller a
        ; segment register and give nothing back for it.  It is not a
        ; theoretical cost: this handler DID zero ES, and the next
        ; thing the caller did was read a sector to ES:BX, which
        ; landed on the real-mode stack at 0F5Ch and took the machine
        ; down two calls later.  Every hard-disk BIOS preserves ES
        ; here anyway.
        xor     eax,eax
        jmp     i13_ok

;=====================================================================
; AH=15h - what kind of drive is this?
;
; The answer for a hard disk carries its size in CX:DX, and that one
; IS the true capacity: it is a 32-bit sector count with no ten-bit
; field to squeeze through.
;=====================================================================
i13_type:
        mov     al,[i13_drive]
        cmp     al,80h
        jae     i13_tfixed

        ; A floppy, and we cannot tell whether it has a change line
        ; without asking the controller, so we say the safe thing: the
        ; drive that has none.  A program told there is one waits for
        ; a signal that never comes; a program told there is not
        ; re-reads the boot sector, which is only slow.
        mov     ah,D13_T_FLOPPY
        jmp     short i13_tdone

i13_tfixed:
        mov     al,[i13_drive]
        call    dword ptr ds:[IOS_DRVPARM]      ; EBX = true sector count
        jc      i13_badcmd
        mov     word ptr [vdm_ctx+V86C_EDX],bx
        shr     ebx,16
        mov     word ptr [vdm_ctx+V86C_ECX],bx
        mov     ah,D13_T_FIXED
i13_tdone:
        mov     byte ptr [vdm_ctx+V86C_EAX+1],ah
        jmp     i13_clc

;=====================================================================
; AH=41h - are the extensions here?  They are.
;
; Worth having even in a 16-bit box: CHS runs out at 1024 cylinders,
; and the kernel's own block layer has been LBA underneath all along,
; so the extended calls are less work to serve than the packed ones.
;=====================================================================
i13_eddchk:
        cmp     word ptr [vdm_ctx+V86C_EBX],EDD_MAGIC
        jne     i13_badcmd
        mov     word ptr [vdm_ctx+V86C_EBX],EDD_REPLY
        mov     word ptr [vdm_ctx+V86C_ECX],EDD_CAP_EXT
        mov     byte ptr [vdm_ctx+V86C_EAX+1],EDD_VER11
        jmp     i13_clc

;=====================================================================
; AH=42h / 43h - extended read and write, through a disk address
;                packet at DS:SI.
;=====================================================================
i13_extread:
        mov     byte ptr [i13_iswrite],0
        jmp     short i13_ext
i13_extwrite:
        mov     byte ptr [i13_iswrite],1

i13_ext:
        mov     eax,dword ptr [vdm_ctx+V86C_DS]
        shl     eax,4
        movzx   edx,word ptr [vdm_ctx+V86C_ESI]
        add     eax,edx
        mov     [i13_dap],eax
        mov     esi,eax

        movzx   eax,byte ptr [esi+DAP_SIZE]
        cmp     eax,DAP_LEN
        jb      i13_badcmd

        movzx   ecx,word ptr [esi+DAP_COUNT]
        or      ecx,ecx
        jz      i13_extnone
        mov     [i13_count],ecx

        ; 64 bits of LBA, of which this kernel serves the low 32.  A
        ; disk with more than two thousand million sectors on it is not
        ; one PM-DOS is going to be asked about, and answering half of
        ; the address would be worse than declining all of it.
        cmp     dword ptr [esi+DAP_LBA+4],0
        jne     i13_badsec
        mov     eax,dword ptr [esi+DAP_LBA]
        mov     [i13_lba],eax

        ; The buffer, and ONLY the segment:offset form.  A packet of
        ; 18h bytes with FFFF:FFFF in the far pointer means "use the
        ; 64-bit flat address behind it", and a flat address from a
        ; guest is an address that can name the kernel's own memory.
        ; The far pointer cannot: sixteen times a segment plus an
        ; offset stops just past the megabyte, inside the box.
        mov     eax,dword ptr [esi+DAP_BUF]
        cmp     eax,0FFFFFFFFh
        je      i13_badcmd
        mov     edx,eax
        shr     edx,16                          ; segment
        shl     edx,4
        movzx   eax,ax                          ; offset
        add     eax,edx
        mov     [i13_buf],eax
        call    i13_bufok
        jc      i13_badbuf

        ; the range, against the disk's true capacity this time
        mov     al,[i13_drive]
        call    dword ptr ds:[IOS_DRVPARM]
        jc      i13_badsec
        mov     eax,[i13_lba]
        add     eax,[i13_count]
        jc      i13_badsec
        cmp     eax,ebx
        ja      i13_badsec

        call    i13_flush
        mov     al,[i13_drive]
        mov     ebx,[i13_lba]
        mov     ecx,[i13_count]
        mov     edi,[i13_buf]
        cmp     byte ptr [i13_iswrite],0
        je      i13_extrd
        call    dword ptr ds:[IOS_WRBLK]
        jc      i13_ioerr
        call    i13_forget
        jmp     short i13_extdone
i13_extrd:
        call    dword ptr ds:[IOS_RDBLK]
        jc      i13_ioerr
i13_extdone:
        ; the packet reports back how many sectors actually moved
        mov     esi,[i13_dap]
        mov     eax,[i13_count]
        mov     word ptr [esi+DAP_COUNT],ax
i13_extnone:
        xor     eax,eax
        jmp     i13_ok

;=====================================================================
; AH=48h - the extended drive parameters, into the caller's buffer.
;=====================================================================
i13_extparams:
        mov     eax,dword ptr [vdm_ctx+V86C_DS]
        shl     eax,4
        movzx   edx,word ptr [vdm_ctx+V86C_ESI]
        add     eax,edx
        mov     edi,eax

        movzx   eax,word ptr [edi+DP_SIZE]      ; the caller says how much
        cmp     eax,DP_MINLEN                   ; room there is, and a
        jb      i13_badcmd                      ; short one gets nothing

        mov     word ptr [edi+DP_SIZE],DP_MINLEN
        mov     eax,[i13_heads]
        mov     dword ptr [edi+DP_HEADS],eax
        mov     eax,[i13_spt]
        mov     dword ptr [edi+DP_SPT],eax

        mov     eax,[i13_chstotal]
        xor     edx,edx
        mov     ecx,[i13_heads]
        imul    ecx,[i13_spt]
        or      ecx,ecx                         ; JECXZ only reaches 127 bytes
        jz      i13_badcmd
        div     ecx
        mov     dword ptr [edi+DP_CYLS],eax

        mov     al,[i13_drive]
        call    dword ptr ds:[IOS_DRVPARM]      ; EBX = sectors, CX = bytes
        jc      i13_badcmd
        mov     dword ptr [edi+DP_TOTAL],ebx
        mov     dword ptr [edi+DP_TOTAL+4],0
        mov     word ptr [edi+DP_BPS],cx

        ; Flags: the CHS fields above are valid, and a floppy is also
        ; removable and has a change line as far as anything cares.
        mov     eax,0002h
        cmp     byte ptr [i13_drive],80h
        jae     i13_dpflags
        or      eax,0014h
i13_dpflags:
        mov     word ptr [edi+DP_FLAGS],ax
        xor     eax,eax
        jmp     i13_ok

;=====================================================================
; The ways out.  Every one of them leaves the status where the real
; BIOS leaves it - in AH, in the carry flag, and in the BIOS data area
; - because programs read all three and some read only the third.
;
; They END THE INTERRUPT rather than return: this handler is reached
; by a jump from the monitor's dispatcher, in the same way as the
; INT 15h and INT 2Fh ones, and the way back is to the monitor loop.
;=====================================================================
i13_badcmd:
        mov     ah,D13_BADCMD
        jmp     short i13_fail
i13_badsec:
        mov     ah,D13_BADSEC
        jmp     short i13_fail
i13_badbuf:
        mov     ah,D13_BADBUF                   ; "your buffer, not ours"
        jmp     short i13_fail
i13_ioerr:
        or      ah,ah                           ; the block layer's own code,
        jnz     i13_fail                        ; unless it did not give one
        mov     ah,D13_GENERAL

i13_fail:
IFDEF I13_DEBUG
        push    eax
        mov     esi,OFFSET msgI13F
        call    dword ptr ds:[IOS_CONSTR]
        pop     eax
        push    eax
        movzx   eax,ah
        call    vdm_hex8
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        pop     eax
ENDIF
        mov     byte ptr ds:[BDA_DSKSTAT],ah
        mov     byte ptr [vdm_ctx+V86C_EAX+1],ah
        mov     byte ptr [vdm_ctx+V86C_EAX],0   ; nothing was transferred
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

; AL = sectors transferred, where the call had a count at all.
i13_ok:
        mov     byte ptr ds:[BDA_DSKSTAT],D13_OK
        mov     byte ptr [vdm_ctx+V86C_EAX],al
        mov     byte ptr [vdm_ctx+V86C_EAX+1],D13_OK
i13_clc:
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

;=====================================================================
; i13_bufok - is [i13_buf, + i13_count sectors) somewhere this guest is
;             entitled to have a sector put?  CF set if it is not.
;
; On a real machine a program that reads a sector over the interrupt
; vector table destroys its own machine and that is its own business.
; Here it would destroy OURS: below the DOS box's arena sit the
; real-mode stack, the thunk's register frame, the transfer buffer the
; block layer stages BIOS I/O through, and the relocated real-mode
; stub - none of which belong to the box, and all of which the kernel
; is standing on while it services the very call that would overwrite
; them.  The failure is not even local: the sector lands, the transfer
; reports success, and the machine dies at the NEXT thunk with an
; instruction pointer in the middle of the vector table.
;
; That is precisely what a zeroed ES did during this handler's first
; hour, so the guard is not hypothetical.  Everything from V86_TPA up
; to the megabyte is the box's - which still leaves 0000:7C00, where
; every boot-sector utility ever written puts its sector.
;=====================================================================
i13_bufok:
        push    eax
        push    edx
        mov     eax,[i13_buf]
        cmp     eax,V86_TPA
        jb      i13_bufbad
        mov     edx,[i13_count]
        shl     edx,9                           ; sectors -> bytes
        add     eax,edx
        jc      i13_bufbad
        cmp     eax,00100000h                   ; and not past the megabyte
        ja      i13_bufbad
        pop     edx
        pop     eax
        clc
        ret
i13_bufbad:
        pop     edx
        pop     eax
        stc
        ret

;=====================================================================
; i13_chs2lba - the CHS triple in the guest's CX and DX, turned into
;               the LBA the block layer counts in.
;
;   lba = (cylinder * heads + head) * sectors_per_track + sector - 1
;
; Every field is checked against the geometry rather than trusted.  A
; head or sector number outside it does not wrap round onto a
; neighbouring track the way the arithmetic alone would have it - it
; is the error the BIOS returns, which is how a program discovers
; where the disk ends.
;=====================================================================
i13_chs2lba:
        push    ebx
        push    ecx
        push    edx

        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        movzx   ebx,cl
        and     ebx,3Fh                         ; CL 5:0 = sector, 1-based
        or      ebx,ebx
        jz      i13_c2lbad
        cmp     ebx,[i13_spt]
        ja      i13_c2lbad
        dec     ebx                             ; ...0-based from here

        mov     eax,ecx
        shr     eax,8
        and     eax,0FFh                        ; CH = cylinder bits 7:0
        and     ecx,0C0h
        shl     ecx,2                           ; CL 7:6 = bits 9:8
        or      eax,ecx                         ; EAX = cylinder

        movzx   edx,byte ptr [vdm_ctx+V86C_EDX+1]       ; DH = head
        cmp     edx,[i13_heads]
        jae     i13_c2lbad

        imul    eax,[i13_heads]
        add     eax,edx
        imul    eax,[i13_spt]
        add     eax,ebx
        mov     [i13_lba],eax

        ; and the whole transfer has to fit inside the disk
        add     eax,[i13_count]
        cmp     eax,[i13_chstotal]
        ja      i13_c2lbad

        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
i13_c2lbad:
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; i13_flush / i13_forget - the two halves of keeping the filesystem
; and a raw transfer in step, over the range this call is about.
;
; The order is the whole of it.  Flush BEFORE, so that a read sees
; what the kernel believes and a write is not later undone by a cache
; being written out over the top of it; forget AFTER a write, when the
; sectors on the medium have become somebody else's and every copy we
; hold of them is a copy of what used to be there.
;=====================================================================
i13_flush:
        pushad
        mov     al,[i13_drive]
        mov     ah,0
        mov     ebx,[i13_lba]
        mov     ecx,[i13_count]
        call    vol_syncdrv
        popad
        ret

i13_forget:
        pushad
        mov     al,[i13_drive]
        mov     ah,1
        mov     ebx,[i13_lba]
        mov     ecx,[i13_count]
        call    vol_syncdrv
        popad
        ret

;=====================================================================
; i13_count_drives - how many floppies and how many hard disks the
; machine has, for AH=08h to report in DL.
;
; The BIOS counted them at power-on and wrote both numbers down: the
; hard disks in the BIOS data area, the floppies in the equipment word
; two bits at a time.  Counting them again ourselves would mean
; probing drives that may not be there, which is slow on a floppy
; controller and audible.
;
; Called once at boot; a machine does not grow a disk while it runs.
;=====================================================================
i13_count_drives:
        push    eax
        movzx   eax,word ptr ds:[BDA_EQUIP]
        test    al,1                            ; bit 0: any floppies?
        jz      i13_nofd
        shr     eax,6
        and     eax,3                           ; bits 7:6 = count, less one
        inc     eax
        jmp     short i13_fdok
i13_nofd:
        xor     eax,eax
i13_fdok:
        mov     [i13_fdcount],al
        mov     al,byte ptr ds:[BDA_HDCOUNT]
        mov     [i13_hdcount],al
        pop     eax
        ret

;=====================================================================
IFDEF I13_DEBUG
msgI13      db  13,10,'[13:',0
msgI13F     db  ' ERR ',0
msgI13L     db  ' lba=',0
msgI13B     db  ' buf=',0
msgI13N     db  ' n=',0
ENDIF
i13_fn          db  0                   ; the function, as it came in
i13_drive       db  0                   ; and the BIOS drive number
i13_iswrite     db  0                   ; 0 read, 1 write, 2 verify
i13_fdcount     db  0
i13_hdcount     db  0
                db  0,0,0
i13_spt         dd  0                   ; the geometry, per call
i13_heads       dd  0
i13_chstotal    dd  0                   ; sectors CHS can address
i13_lba         dd  0                   ; the transfer, per call
i13_count       dd  0
i13_buf         dd  0
i13_dap         dd  0                   ; and the packet it came from
i13_scratch     db  512 dup(0)          ; where a verify throws sectors
