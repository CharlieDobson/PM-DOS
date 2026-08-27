;=====================================================================
; BLOCK.INC - block device driver
;
; Sector I/O for PMDOS, over two paths:
;
;   ATA PIO   hard disks that answered IDENTIFY (see ATA.INC).  The
;             transfer lands straight in the caller's flat buffer.
;   BIOS      everything else - floppies always, and hard disks on
;             machines with no IDE controller to talk to, which is
;             what keeps DOSBox 0.74 working.  Staged through
;             XFER_BUF below 1M, where the BIOS can reach it.
;
; Drive numbers are BIOS numbers (00h = A:, 80h = first hard disk).
; CHS geometry is fetched once per drive via INT 13h/08h and cached;
; the ATA path needs none of it.
;
; Included by IOPM.ASM (after ATA.INC).
;=====================================================================

MAX_DRIVES      EQU     4               ; slots: A:, B:, hd0, hd1

; per-drive geometry cache entry
GEO_SPT         EQU     0               ; word - sectors per track
GEO_HEADS       EQU     2               ; word - head count
GEO_TOTAL       EQU     4               ; dword - total sectors BY CHS
GEO_FLAGS       EQU     8               ; word - 1 = valid
GEO_EXT         EQU     10              ; word - 1 = INT 13h extensions
GEO_LBATOT      EQU     12              ; dword - true capacity, 0 = unknown
GEO_LEN         EQU     16

;=====================================================================
; blk_init - forget any cached geometry, then look for IDE disks.
;=====================================================================
blk_init:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET geo_cache
        xor     eax,eax
        mov     ecx,(MAX_DRIVES * GEO_LEN + 3) / 4
        rep     stosd
        call    ata_init
        pop     edi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; blk_slot - EBX = geometry cache entry for drive AL, CF set if the
;            drive number has no slot.
;---------------------------------------------------------------------
blk_slot:
        push    eax
        cmp     al,2                    ; floppies 0 and 1
        jb      bs_have
        cmp     al,80h
        jb      bs_bad
        cmp     al,82h                  ; hard disks 80h and 81h
        jae     bs_bad
        sub     al,80h - 2
bs_have:
        movzx   ebx,al
        imul    ebx,GEO_LEN
        add     ebx,OFFSET geo_cache
        pop     eax
        clc
        ret
bs_bad:
        pop     eax
        stc
        ret

;=====================================================================
; blk_getgeo - make sure the geometry for drive AL is cached.
;              EBX = cache entry on return.  CF set on failure.
;=====================================================================
blk_getgeo:
        call    blk_slot
        jc      bg_out
        test    word ptr [ebx+GEO_FLAGS],1
        jnz     bg_ok

        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebx
        mov     [bg_drive],al           ; 08h answers in DL and EAX is scratch
                                        ; from here on, so the number this
                                        ; call is about is kept aside
        call    rm_clear                ; INT 13h/08h - drive parameters
        mov     byte ptr ds:[RMR_EAX+1],08h
        mov     byte ptr ds:[RMR_EDX],al
        mov     al,13h
        call    rm_int
        call    rm_cf
IFDEF FDD_DEBUG
        pushfd
        push    eax
        mov     esi,OFFSET msgGeo
        call    con_str
        movzx   eax,byte ptr ds:[RMR_EDX]
        call    con_hex8
        mov     al,'/'
        call    con_out
        movzx   eax,byte ptr ds:[RMR_EAX+1]     ; AH = status
        call    con_hex8
        mov     al,'/'
        call    con_out
        mov     eax,dword ptr ds:[RMR_FLAGS]
        call    con_hex8
        mov     al,'/'
        call    con_out
        movzx   eax,word ptr ds:[RMR_ECX]
        call    con_hex16
        mov     al,'/'
        call    con_out
        movzx   eax,byte ptr ds:[RMR_EDX+1]
        call    con_hex8
        mov     al,' '
        call    con_out
        pop     eax
        popfd
ENDIF
        pop     ebx
        jnz     bg_fail

        movzx   ecx,byte ptr ds:[RMR_ECX]       ; CL: sector count in 5:0,
        and     ecx,3Fh                         ; cyl bits 9:8 in 7:6
        mov     [ebx+GEO_SPT],cx

        movzx   edx,byte ptr ds:[RMR_EDX+1]     ; DH = max head
        inc     edx
        mov     [ebx+GEO_HEADS],dx

        movzx   eax,byte ptr ds:[RMR_ECX+1]     ; CH = low 8 cylinder bits
        movzx   esi,byte ptr ds:[RMR_ECX]
        shl     esi,2
        and     esi,300h
        or      eax,esi
        inc     eax                             ; EAX = cylinders
        imul    eax,edx
        imul    ecx
        mov     [ebx+GEO_TOTAL],eax
        mov     word ptr [ebx+GEO_FLAGS],1

        call    blk_edd                 ; and does it know 42h as well?

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
bg_ok:
        clc
bg_out:
        ret

bg_fail:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; blk_edd - does the BIOS know AH=42h for the drive in bg_drive?
;           EBX = its cache entry.  Fills GEO_EXT and GEO_LBATOT.
;
; THE HALF OF THE EXTENSIONS THAT WAS MISSING.  A DOS box has had
; 41h/42h/43h/48h since v0.27, out of DOS\INT13.INC - but that is the
; kernel ANSWERING them, translated into the flat sector I/O below.
; This is the kernel ASKING them, and without it every BIOS transfer
; was a CHS one: a cylinder number ten bits wide, which stops at 1024
; cylinders and, with sixteen heads and 63 sectors, at 504 megabytes.
;
; It matters only where ATA.INC is not already doing the work.  An IDE
; disk that answered IDENTIFY is addressed directly and in LBA and
; always has been; this is the machine whose controller PM-DOS does not
; drive - a SCSI card with an INT 13h ROM, a BIOS-only build, DOSBox -
; carrying a disk larger than the CHS geometry can name.
;
; FLOPPIES ARE NOT ASKED.  41h on drive 00h on a BIOS that does not
; have the extensions returns whatever was in the registers, and there
; is no floppy anywhere near the horizon these calls exist to cross.
;=====================================================================
blk_edd:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     word ptr [ebx+GEO_EXT],0
        mov     dword ptr [ebx+GEO_LBATOT],0
        cmp     byte ptr [bg_drive],80h
        jb      be_out

        call    rm_clear                ; AH=41h BX=55AAh - are you there?
        mov     byte ptr ds:[RMR_EAX+1],41h
        mov     word ptr ds:[RMR_EBX],EDD_MAGIC
        mov     al,[bg_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     al,13h
        call    rm_int
        call    rm_cf
        jnz     be_out                  ; CF: no extensions here
        cmp     word ptr ds:[RMR_EBX],EDD_REPLY
        jne     be_out                  ; BX not transformed: nor here
        test    byte ptr ds:[RMR_ECX],EDD_CAP_EXT
        jz      be_out                  ; present, but not the packet calls
        mov     word ptr [ebx+GEO_EXT],1

        ; AH=48h - the capacity, which is the other thing CHS cannot
        ; say.  A drive can have the extensions and refuse this, so
        ; the answer is optional and GEO_TOTAL stays the fallback.
        push    ebx
        mov     edi,GV_EDP
        xor     eax,eax
        mov     ecx,32 / 4
        rep     stosd
        mov     word ptr ds:[GV_EDP+DP_SIZE],DP_MINLEN
        call    rm_clear
        mov     byte ptr ds:[RMR_EAX+1],48h
        mov     al,[bg_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     word ptr ds:[RMR_DS],IO_SEG
        mov     dword ptr ds:[RMR_ESI],GVO_EDP
        mov     al,13h
        call    rm_int
        call    rm_cf
        pop     ebx
        jnz     be_out
        cmp     dword ptr ds:[GV_EDP+DP_TOTAL+4],0
        jne     be_huge                 ; past 2TB: keep what CHS said and
                                        ; let the partition table decide
        mov     eax,dword ptr ds:[GV_EDP+DP_TOTAL]
        mov     [ebx+GEO_LBATOT],eax
be_huge:
be_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; blk_params - IOS_DRVPARM.  AL = drive.
;              Returns EBX = total sectors, CX = bytes per sector.
;              CF set if the drive does not exist.
;=====================================================================
blk_params:
        call    ata_capacity            ; IDENTIFY knows better than CHS
        jnc     bp_ata
        call    blk_getgeo
        jc      bp_out
        cmp     dword ptr [ebx+GEO_LBATOT],0
        je      bp_chs
        mov     ebx,[ebx+GEO_LBATOT]    ; and so does AH=48h
        jmp     short bp_ata
bp_chs:
        mov     ebx,[ebx+GEO_TOTAL]
bp_ata:
        mov     cx,512
        clc
bp_out:
        ret

;=====================================================================
; blk_geo - IOS_DRVGEO.  AL = drive.
;           Returns CX = sectors per track, DX = heads,
;                   EBX = total sectors ACCORDING TO THAT GEOMETRY.
;           CF set if the drive does not exist.
;
; Deliberately NOT blk_params: this is the BIOS's CHS description of
; the disk, cylinders truncated at 1024 and all, and that is exactly
; what a DOS program doing INT 13h has been told to expect.  Answering
; with the true capacity would hand a guest cylinder numbers its own
; arithmetic cannot express.
;=====================================================================
blk_geo:
        push    esi
        call    blk_getgeo
        jc      bgo_fail
        mov     esi,ebx
        movzx   ecx,word ptr [esi+GEO_SPT]
        movzx   edx,word ptr [esi+GEO_HEADS]
        or      ecx,ecx                 ; a drive that answered 08h with
        jz      bgo_fail                ; no sectors per track is not one
        or      edx,edx                 ; we can translate for
        jz      bgo_fail
        mov     ebx,[esi+GEO_TOTAL]
        pop     esi
        clc
        ret
bgo_fail:
        pop     esi
        stc
        ret

;=====================================================================
; blk_read - IOS_RDBLK.
;   AL  = drive        EBX = starting LBA
;   CX  = sector count EDI = destination (flat)
;   CF set on error, AH = BIOS status.
;=====================================================================
blk_read:
        call    ata_owns                ; is this one of the IDE disks?
        jc      blk_r_bios
        push    ecx
        movzx   ecx,cx
        call    ata_read
        pop     ecx
        jmp     short blk_ata_ret
blk_r_bios:
        mov     byte ptr [bio_write],0
        jmp     short blk_go

;=====================================================================
; blk_write - IOS_WRBLK, same registers, EDI = source.
;=====================================================================
blk_write:
        call    ata_owns
        jc      blk_w_bios
        push    ecx
        movzx   ecx,cx
        call    ata_write
        pop     ecx
        jmp     short blk_ata_ret
blk_w_bios:
        mov     byte ptr [bio_write],1
        jmp     short blk_go

;--- an ATA disk answered, for better or worse.  A failure here is a
;--- real failure, not a reason to go asking the BIOS instead.
blk_ata_ret:
        jc      blk_ata_err
        xor     ah,ah
        clc
        ret
blk_ata_err:
        mov     ah,0Ch                  ; BIOS-style "general failure"
        stc
        ret

;---------------------------------------------------------------------
blk_go:
        pushad
        mov     [bio_drive],al
        mov     [bio_lba],ebx
        movzx   ecx,cx
        mov     [bio_count],ecx
        mov     [bio_buf],edi

        call    blk_getgeo
        jc      bio_geofail
        mov     [bio_geo],ebx

bio_loop:
        cmp     dword ptr [bio_count],0
        je      bio_done

;--- LBA -> CHS -------------------------------------------------------
        mov     ebx,[bio_geo]
        mov     eax,[bio_lba]
        xor     edx,edx
        movzx   ecx,word ptr [ebx+GEO_SPT]
        div     ecx                     ; EAX = track index, EDX = sector-1
        mov     esi,edx                 ; ESI = sector - 1
        xor     edx,edx
        movzx   ecx,word ptr [ebx+GEO_HEADS]
        div     ecx                     ; EAX = cylinder, EDX = head

        ; how many sectors can this call move without leaving the track?
        movzx   ecx,word ptr [ebx+GEO_SPT]
        sub     ecx,esi                 ; room left on the track
        cmp     ecx,[bio_count]
        jbe     bio_n_ok
        mov     ecx,[bio_count]
bio_n_ok:
        cmp     ecx,XFER_SIZE / 512     ; and the staging buffer caps it
        jbe     bio_n_ok2
        mov     ecx,XFER_SIZE / 512
bio_n_ok2:
        mov     [bio_chunk],ecx

        ; CAN CHS SAY WHERE THIS IS?  The cylinder goes into ten bits
        ; and stops there, so a sector past cylinder 1023 has no CHS
        ; address at all - and until this release it got one anyway,
        ; the high bits quietly dropped, which reads or WRITES some
        ; entirely different part of the disk and reports success.
        ; Either the extensions can name it or nobody can.
        mov     byte ptr [bio_ext],0
        cmp     eax,1023
        jbe     bio_stage
        cmp     word ptr [ebx+GEO_EXT],0
        je      bio_nocyl
        mov     byte ptr [bio_ext],1

;--- writes: copy the payload down into the staging buffer first ------
bio_stage:
        cmp     byte ptr [bio_write],0
        je      bio_setup
        push    esi
        push    edi
        push    ecx
        mov     esi,[bio_buf]
        mov     edi,XFER_BUF
        shl     ecx,7                   ; sectors -> dwords
        rep     movsd
        pop     ecx
        pop     edi
        pop     esi

;--- build the INT 13h register frame ---------------------------------
bio_setup:
        cmp     byte ptr [bio_ext],0
        jne     bio_extio
        push    eax
        push    edx
        call    rm_clear
        pop     edx
        pop     eax

        mov     byte ptr ds:[RMR_EAX],cl        ; AL = sector count
        mov     cl,02h                          ; AH = 02 read / 03 write
        cmp     byte ptr [bio_write],0
        je      bio_fn
        mov     cl,03h
bio_fn:
        mov     byte ptr ds:[RMR_EAX+1],cl

        mov     ecx,esi
        inc     ecx                             ; CL 5:0 = sector number
        mov     ebx,eax
        shr     ebx,2
        and     bl,0C0h                         ; CL 7:6 = cyl bits 9:8
        or      cl,bl
        mov     byte ptr ds:[RMR_ECX],cl
        mov     byte ptr ds:[RMR_ECX+1],al      ; CH = cyl bits 7:0

        mov     byte ptr ds:[RMR_EDX+1],dl      ; DH = head
        mov     al,[bio_drive]
        mov     byte ptr ds:[RMR_EDX],al        ; DL = drive

        mov     word ptr ds:[RMR_ES],XFER_SEG   ; ES:BX = staging buffer
        mov     dword ptr ds:[RMR_EBX],0

        mov     al,13h
        call    rm_int
        call    rm_cf
IFDEF FDD_DEBUG
        pushfd
        push    eax
        mov     esi,OFFSET msgRd
        call    con_str
        movzx   eax,byte ptr ds:[RMR_EAX+1]     ; AH = status
        call    con_hex8
        mov     al,'/'
        call    con_out
        movzx   eax,byte ptr ds:[RMR_EAX]       ; AL = sectors moved
        call    con_hex8
        mov     al,' '
        call    con_out
        pop     eax
        popfd
ENDIF
        jnz     bio_retry               ; CF from the BIOS -> retry logic
        jmp     short bio_lift

;--- the same transfer, addressed by LBA ------------------------------
; A disk address packet says the whole thing in one place: how many
; sectors, where they go, and a sixty-four-bit sector number that no
; amount of geometry is involved in.  The staging buffer and the
; retry are shared with the CHS path above, because neither of them
; has anything to do with how the sector was named.
bio_extio:
        mov     edi,GV_DAP
        xor     eax,eax
        mov     ecx,DAP_LEN / 4
        rep     stosd
        mov     byte ptr ds:[GV_DAP+DAP_SIZE],DAP_LEN
        mov     eax,[bio_chunk]
        mov     word ptr ds:[GV_DAP+DAP_COUNT],ax
        mov     dword ptr ds:[GV_DAP+DAP_BUF],XFER_SEG * 10000h
        mov     eax,[bio_lba]
        mov     dword ptr ds:[GV_DAP+DAP_LBA],eax

        call    rm_clear
        mov     al,42h                          ; AH = 42 read / 43 write,
        cmp     byte ptr [bio_write],0          ; and AL = 0 on a write means
        je      bio_extfn                       ; "no verify" - ours is done
        mov     al,43h                          ; a level up, in dio_verify
bio_extfn:
        mov     byte ptr ds:[RMR_EAX+1],al
        mov     al,[bio_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     word ptr ds:[RMR_DS],IO_SEG     ; DS:SI = the packet
        mov     dword ptr ds:[RMR_ESI],GVO_DAP
        mov     al,13h
        call    rm_int
        call    rm_cf
        jnz     bio_retry

;--- reads: lift the data out of the staging buffer -------------------
bio_lift:
        cmp     byte ptr [bio_write],0
        jne     bio_advance
        push    esi
        push    edi
        push    ecx
        mov     esi,XFER_BUF
        mov     edi,[bio_buf]
        mov     ecx,[bio_chunk]
        shl     ecx,7
        rep     movsd
        pop     ecx
        pop     edi
        pop     esi

bio_advance:
        mov     eax,[bio_chunk]
        add     [bio_lba],eax
        sub     [bio_count],eax
        shl     eax,9
        add     [bio_buf],eax
        mov     byte ptr [bio_tries],0
        jmp     bio_loop

;--- error: reset the controller and retry the same chunk -------------
bio_retry:
        inc     byte ptr [bio_tries]
        cmp     byte ptr [bio_tries],3
        jae     bio_fail

        call    rm_clear                ; INT 13h/00h - reset
        mov     al,[bio_drive]
        mov     byte ptr ds:[RMR_EDX],al
        mov     al,13h
        call    rm_int
        jmp     bio_loop

bio_fail:
        mov     al,byte ptr ds:[RMR_EAX+1]      ; BIOS status code
        mov     [bio_status],al
        mov     byte ptr [bio_tries],0
        popad
        mov     ah,[bio_status]
        stc
        ret

bio_geofail:
        popad
        mov     ah,0FFh                 ; "no such drive"
        stc
        ret

; Past the CHS horizon on a BIOS that cannot go there.  This is a real
; refusal and not a retry: nothing about trying again makes cylinder
; 1024 expressible in ten bits.  04h is "sector not found", which is
; the truest of the INT 13h status codes for a sector this machine has
; no way to ask for.
bio_nocyl:
        popad
        mov     ah,04h
        stc
        ret

bio_done:
        popad
        xor     ah,ah
        clc
        ret

;=====================================================================
IFDEF FDD_DEBUG
msgGeo      db  '[g8 dl/ah/fl/cx/dh=',0
msgRd       db  '[rd ah/al=',0
ENDIF

geo_cache   db  MAX_DRIVES * GEO_LEN dup(0)

bg_drive    db  0                       ; the drive blk_getgeo is asking about
bio_drive   db  0
bio_write   db  0
bio_tries   db  0
bio_status  db  0
bio_ext     db  0                       ; this chunk goes by packet, not CHS
            db  0,0,0
bio_lba     dd  0
bio_count   dd  0
bio_buf     dd  0
bio_chunk   dd  0
bio_geo     dd  0
