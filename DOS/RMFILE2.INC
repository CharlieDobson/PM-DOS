;=====================================================================
; RMFILE2.INC - handles, and the file calls a program actually makes
;
; A HANDLE IS AN INDEX INTO A TABLE IN THE PSP, which is an index into
; a table here.  That double indirection is not decoration: it is what
; makes redirection survive an EXEC, because a child inherits its
; parent's first five JFT entries and they point at the same open
; files rather than at copies of them.
;=====================================================================

;=====================================================================
; rd_sftinit - the handle table, and the five handles every process
;              starts with.
;=====================================================================
rd_sftinit:
        pusha
        push    es
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rd_sfts
        mov     cx,SFT_COUNT * SFT_LEN
        xor     al,al
        cld
        rep     stosb

        ; The three devices, opened once and shared.  CON is two
        ; handles because standard input and standard output are two
        ; handles, and closing one must not close the other.
        mov     si,OFFSET rd_sfts
        call    sfti_con                ; 0 - stdin
        add     si,SFT_LEN
        call    sfti_con                ; 1 - stdout
        add     si,SFT_LEN
        call    sfti_con                ; 2 - stderr
        add     si,SFT_LEN
        mov     al,DEV_AUX
        call    sfti_dev                ; 3
        add     si,SFT_LEN
        mov     al,DEV_PRN
        call    sfti_dev                ; 4

        ; And the process's own table, which points at them
        mov     es,[h_psp]
        mov     di,18h
        mov     cx,JFT_COUNT
        mov     al,0FFh
        rep     stosb
        mov     di,18h
        xor     al,al
sfti_jft:
        mov     es:[di],al
        inc     di
        inc     al
        cmp     al,5
        jb      sfti_jft
        mov     word ptr es:[32h],JFT_COUNT
        mov     word ptr es:[34h],18h
        mov     ax,[h_psp]
        mov     word ptr es:[36h],ax

        pop     es
        popa
        ret

sfti_con:
        mov     al,DEV_CON
sfti_dev:
        mov     word ptr [si+SFT_REF],1
        mov     byte ptr [si+SFT_FLAGS],SFF_DEVICE
        mov     [si+SFT_DRIVE],al
        call    sft_devname
        call    sft_setdev
        mov     word ptr [si+SFT_MODE],2
        ret

;--- sft_setdev - AL = a device index, SI = its entry: the device
;--- (not sft_devinfo: wasm folds case, and that is the equate above)
;--- information word 4400h answers with, and the far pointer to the
;--- device's header in the block below 64K (RMINFO.INC, rd_sysinit).
sft_setdev:
        push    ax
        push    bx
        mov     word ptr [si+SFT_DEVINFO],8080h  ; a character device
        movzx   bx,al
        imul    bx,bx,RL_DEVLEN
        add     bx,RL_DEVS                      ; CON, AUX, PRN in that order
        cmp     al,DEV_CON
        jne     sdi_notcon
        mov     word ptr [si+SFT_DEVINFO],80D3h  ; ...the console, with
sdi_notcon:                                     ; standard in and out
        cmp     al,DEV_NUL
        jne     sdi_notnul
        mov     word ptr [si+SFT_DEVINFO],8084h  ; ...the bit bucket
        mov     bx,RL_NULDEV
sdi_notnul:
        mov     [si+SFT_DEVPTR],bx
        mov     ax,[rd_lowseg]
        mov     [si+SFT_DEVPTR+2],ax
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
; sft_devname - SI = an SFT entry, AL = DEV_*.  Writes the eleven byte
;               blank padded 8.3 name into SFT_NAME.
;
; NOTHING INSIDE THIS DOS READS THAT FIELD, which is exactly why it
; sat declared and unwritten from v0.45 to v0.52 without anything
; noticing.  The Windows KERNEL reads it, and not by asking:
;
;   it opens CON five times to plant handles, then allocates an LDT
;   descriptor with base 0 and SCANS memory, 64K window by 64K window,
;   for the literal four characters "CON " - and when it finds two
;   copies within 100h bytes of each other it takes the distance
;   between them as the size of an SFT entry.
;
; DOS 3.x is never scanned because the size is a constant there - 35h,
; or 38h for 3.0 - and KRNL386 has both spelt out.  Anything reporting
; 4.0 or later gets measured, and RMDOS reports 6.22.  A DOS whose SFT
; carries no names is a DOS that cannot be measured, the measurement
; returns FFFFh, and the kernel prints "Incorrect MS-DOS version.
; MS-DOS 3.1 or greater required." and exits - which is what it says
; when the DOS is unusable, and has nothing to do with the version.
;
; THE NAME IS COPIED OUT OF rd_devtab AND NOT OUT OF A TABLE OF ITS
; OWN, and that is not a saving - it is the whole point.  The first
; version of this routine kept its own eleven byte copies next to
; rd_devtab, and the two literals landed 99 bytes apart in the image:
; two copies of "CON " inside the 100h window, in a DOS whose SFT is
; measured by the distance between two copies of "CON ".  A scanner
; reaching them first measures an entry as 99 bytes and then walks the
; file table at a stride that is not a stride.  ANY second copy of
; this string in this segment is a decoy for that scan, so there is
; one, and the eight bytes it needs are the eight bytes rd_devtab
; already holds.  LPTn and COMn share DEV_PRN and DEV_AUX with the
; first four entries, and the search stops at the first match, so the
; name that reaches the SFT is always the canonical one.
;
; AX, CX, SI, DI and ES are all preserved: every caller is part way
; through building an entry and still needs them.
;---------------------------------------------------------------------
sft_devname:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    cs                      ; DS is already this segment - every
        pop     es                      ; caller has just written the entry
        lea     di,[si+SFT_NAME]        ; through it - so DS:SI and ES:DI
        mov     si,OFFSET rd_devtab     ; below are both this kernel
sdn_find:
        cmp     byte ptr [si],0FFh
        je      sdn_done                ; not in the table: leave it blank
        cmp     byte ptr [si],al
        je      sdn_got
        add     si,9                    ; code byte + eight characters
        jmp     short sdn_find
sdn_got:
        inc     si
        mov     cx,8
        cld
        rep     movsb
        mov     al,' '                  ; and the extension, which an SFT
        mov     cx,3                    ; carries blank and without a dot
        rep     stosb
sdn_done:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret


;=====================================================================
; dev_name - ES:SI an ASCIIZ path -> CF clear and AL = DEV_* if it
;            names a character device.
;
; DOS's rule, and it is a strange one worth stating: a device name is
; recognised ANYWHERE a filename is, in ANY directory, with ANY
; extension and with any path in front of it.  C:\DOES\NOT\EXIST\CON
; is the console.  So this takes the last component, stops at the dot,
; and compares eight-and-a-bit characters against a table - it does
; not look at the path at all, because DOS does not either.
;=====================================================================
dev_name:
        push    cx
        push    dx
        push    di
        push    si

        ; the last component: after the final \ / or :
        mov     di,si
dvn_scan:
        mov     al,es:[di]
        or      al,al
        jz      dvn_end
        cmp     al,'\'
        je      dvn_cut
        cmp     al,'/'
        je      dvn_cut
        cmp     al,':'
        jne     dvn_next
dvn_cut:
        inc     di
        mov     si,di
        dec     di
dvn_next:
        inc     di
        jmp     short dvn_scan
dvn_end:

        ; copy up to eight characters, upper cased, stopping at the dot
        mov     di,OFFSET rd_devname
        mov     cx,8
        push    cx
dvn_blank:
        mov     byte ptr cs:[di],' '
        inc     di
        loop    dvn_blank
        pop     cx
        mov     di,OFFSET rd_devname
dvn_copy:
        mov     al,es:[si]
        or      al,al
        jz      dvn_cmp
        cmp     al,'.'
        je      dvn_cmp
        cmp     al,'a'
        jb      dvn_store
        cmp     al,'z'
        ja      dvn_store
        sub     al,20h
dvn_store:
        mov     cs:[di],al
        inc     si
        inc     di
        loop    dvn_copy

        ; ...and a name longer than eight characters is not a device
        mov     al,es:[si]
        or      al,al
        jz      dvn_cmp
        cmp     al,'.'
        jne     dvn_no

dvn_cmp:
        ; A DRIVER CONFIG.SYS LOADED, FIRST - see drv_find in RMDEV.INC,
        ; which leaves its header in rd_drvhdr for sub_devopen.
        call    drv_find
        jc      dvn_table
        mov     al,DEV_DRV
        pop     si
        pop     di
        pop     dx
        pop     cx
        clc
        ret
dvn_table:
        mov     si,OFFSET rd_devtab
dvn_try:
        mov     al,cs:[si]
        cmp     al,0FFh
        je      dvn_no
        mov     di,OFFSET rd_devname
        mov     cx,8
        push    si
        inc     si
dvn_char:
        mov     al,cs:[si]
        cmp     al,cs:[di]
        jne     dvn_nextent
        inc     si
        inc     di
        loop    dvn_char
        pop     si
        mov     al,cs:[si]              ; the device number
        pop     si
        pop     di
        pop     dx
        pop     cx
        clc
        ret
dvn_nextent:
        pop     si
        add     si,9                    ; number + eight characters
        jmp     short dvn_try
dvn_no:
        pop     si
        pop     di
        pop     dx
        pop     cx
        stc
        ret

;=====================================================================
; sub_devopen - AL = DEV_*, and it becomes an open handle.  The entry
;               is built exactly as sfti_dev builds the inherited
;               ones, because a device opened by name and a device
;               inherited from a parent have to behave identically -
;               everything downstream reads SFF_DEVICE and SFT_DRIVE
;               and nothing else.
;=====================================================================
sub_devopen:
        push    ax
        push    si
        push    di
        mov     [rd_opdev],al
        call    jft_alloc               ; -> BX
        jc      sdo_bad
        mov     [rd_ophnd],bx
        call    sft_alloc               ; -> SI, AL = index
        jc      sdo_bad
        mov     [rd_opidx],al
        mov     word ptr [si+SFT_REF],1
        mov     byte ptr [si+SFT_FLAGS],SFF_DEVICE
        mov     al,[rd_opdev]
        mov     [si+SFT_DRIVE],al
        mov     word ptr [si+SFT_MODE],2
        mov     dword ptr [si+SFT_SIZE],0
        mov     dword ptr [si+SFT_POS],0
        cmp     al,DEV_DRV
        je      sdo_driver
        call    sft_devname
        call    sft_setdev
        jmp     short sdo_jft
sdo_driver:
        ; A LOADED DRIVER: its header, its name, its attributes - and an
        ; OPEN request if the header asks for one, which it may refuse.
        ; Refused, the entry goes back unused and so does the handle,
        ; which has not been written into the table yet.
        call    drv_setsft
        call    drv_open
        jnc     sdo_jft
        mov     word ptr [si+SFT_REF],0
        mov     [rd_opfail],ax
        pop     di
        pop     si
        pop     ax
        mov     ax,[rd_opfail]
        stc
        ret
sdo_jft:
        push    es                      ; the handle points at the entry -
        push    cx                      ; written the same way sub_fromdir
        push    di                      ; writes it, because there is no
        mov     es,[rd_psp]             ; routine for it and inventing one
        call    jft_ptr                 ; for a second caller is how two
        mov     bx,[rd_ophnd]           ; versions of a rule start
        mov     al,[rd_opidx]
        mov     es:[bx+di],al
        pop     di
        pop     cx
        pop     es
        mov     bx,[rd_ophnd]
        pop     di
        pop     si
        pop     ax
        clc
        ret
sdo_bad:
        pop     di
        pop     si
        pop     ax
        stc
        ret

rd_opfail   dw  0
rd_devname  db  8 dup (' ')
rd_devtab   db  DEV_CON,'CON     '
            db  DEV_AUX,'AUX     '
            db  DEV_PRN,'PRN     '
            db  DEV_NUL,'NUL     '
            db  DEV_PRN,'LPT1    '
            db  DEV_PRN,'LPT2    '
            db  DEV_PRN,'LPT3    '
            db  DEV_AUX,'COM1    '
            db  DEV_AUX,'COM2    '
            db  DEV_AUX,'COM3    '
            db  DEV_AUX,'COM4    '
            db  0FFh
rd_opdev    db  0

;=====================================================================
; sft_get - BX = a handle -> SI = its table entry.  CF if the handle
;           is not open.
;=====================================================================
sft_get:
        push    es
        push    ax
        push    cx
        push    di
        mov     es,[rd_psp]
        call    jft_ptr                 ; -> ES:DI the table, CX its size
        cmp     bx,cx
        jae     sg_bad
        mov     al,es:[bx+di]
        cmp     al,0FFh
        je      sg_bad
        cmp     al,SFT_COUNT
        jae     sg_bad
        ; IMUL, AND THAT IS THE WHOLE POINT OF IT.  This was MUL SI,
        ; which is a 16-bit multiply and therefore writes DX:AX - so
        ; sft_get, whose contract is "BX in, SI out, CF on error",
        ; quietly destroyed DX as well.  The product never fills DX,
        ; so what every caller actually saw was DX = 0.
        ;
        ; sub_read takes its buffer offset in DX and read it AFTER
        ; calling this, which is how EXEC came to read every file it
        ; loads into offset zero of the destination segment.  See
        ; sub_read below and f4B_com in RMMEM.INC, where the second
        ; half of that accident is written down.
        movzx   si,al
        imul    si,si,SFT_LEN
        add     si,OFFSET rd_sfts
        cmp     word ptr [si+SFT_REF],0
        je      sg_bad
        pop     di
        pop     cx
        pop     ax
        pop     es
        clc
        ret
sg_bad:
        pop     di
        pop     cx
        pop     ax
        pop     es
        stc
        ret

;=====================================================================
; sft_isnet - SI = a table entry.  ZF if it is a redirector's file:
;             bit 15 of DOS's flags word set and bit 7, a device, clear.
;             Every register is kept.
;
; BIT 15 ALONE IS NOT ENOUGH HERE, though it is in MS-DOS.  This DOS's
; own devices keep in that word what 4400h answers - 80D3h for CON,
; 8080h for AUX and PRN - and 4400h's high byte is the device header's,
; whose bit 15 means "a character device".  MS-DOS keeps the flags alone
; in the entry and builds 4400h's answer when asked; here the answer is
; kept, so a device's bit 15 is set, and the first child to end with
; CON inherited closed it through the redirector (NETTEST, 2026-10-07).
; A redirector's entry is a file, so bit 7 tells them apart.
;=====================================================================
sft_isnet:
        push    ax
        mov     ax,word ptr [si+SFT_DEVINFO]
        and     ax,8080h
        cmp     ax,8000h
        pop     ax
        ret

;--- sft_alloc - a free table entry -> SI, AL = its index.  CF if full
sft_alloc:
        push    cx
        mov     si,OFFSET rd_sfts
        xor     al,al
        mov     cx,SFT_COUNT
sa_loop:
        cmp     word ptr [si+SFT_REF],0
        je      sa_got
        add     si,SFT_LEN
        inc     al
        loop    sa_loop
        pop     cx
        stc
        ret
sa_got:
        push    di
        push    ax
        mov     di,si
        push    es
        push    cs
        pop     es
        mov     cx,SFT_LEN
        push    di
        xor     al,al
        cld
        rep     stosb
        pop     di
        pop     es
        pop     ax
        mov     si,di
        pop     di
        mov     word ptr [si+SFT_REF],1
        pop     cx
        clc
        ret

;--- sft_addref / sft_release - AL = a table index -------------------
sft_addref:
        push    si
        push    ax
        cmp     al,SFT_COUNT
        jae     sar_done
        movzx   si,al
        mov     ax,SFT_LEN
        mul     si
        mov     si,ax
        add     si,OFFSET rd_sfts
        inc     word ptr [si+SFT_REF]
sar_done:
        pop     ax
        pop     si
        ret

; BX = a handle.  Drops one reference, closing the file if it was the
; last - which is the only place a directory entry is written back.
sft_release:
        push    si
        push    ax
        call    sft_get
        jc      srl_done
        call    sft_isnet
        je      srl_net
        cmp     word ptr [si+SFT_REF],0
        je      srl_done
        dec     word ptr [si+SFT_REF]
        cmp     word ptr [si+SFT_REF],0
        jne     srl_done
        call    file_commit
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jz      srl_done
        cmp     byte ptr [si+SFT_DRIVE],DEV_DRV
        jne     srl_done
        call    drv_close               ; the last handle on a loaded driver
srl_done:
        pop     ax
        pop     si
        ret
srl_net:
        ; A REDIRECTOR'S FILE: THE REDIRECTOR DROPS THE COUNT, not this DOS.
        ; MS-DOS's DOS_CLOSE hands a network file's close over whole
        ; (DOS6SRC dos/close.asm) and the redirector frees the entry when
        ; the count reaches nought.  What it answers is not this caller's
        ; business: the handle is gone either way, as it is in DOS.
        pusha
        push    es
        mov     ax,1106h
        call    net_sft
        pop     es
        popa
        jmp     short srl_done

;--- jft_alloc - a free slot in the process's table -> BX ------------
;=====================================================================
; jft_ptr - ES = a PSP -> ES:DI is its handle table and CX is how many
;           entries it has.  ES comes back pointing at the TABLE.
;
; EVERY HANDLE ACCESS GOES THROUGH HERE.  Twelve places used to index
; es:[bx+18h] against a fixed limit of twenty, which is the table DOS
; BUILDS and not the table a program is entitled to.  67h exists so
; that a program can ask for more, DOS answers by moving the table
; into a block of its own, and the two words at PSP:32h and PSP:34h
; are how everything afterwards finds it.  This DOS filled those two
; words in correctly from the first release and then never read them.
;
; AND A PROGRAM THAT ASKS FOR MORE IS NOT EXOTIC.  Windows' KERNEL
; asks at start-up.  Refused, it ran on twenty; the eighteenth file
; was the last it could open, and Program Manager's own PROGMAN.INI
; was the nineteenth - refused, twenty-four times in a row, with "too
; many open files".  That is the whole of the empty Program Manager:
; the shell could not read the file that names its groups.
;=====================================================================
jft_ptr:
        mov     cx,es:[32h]
        les     di,dword ptr es:[34h]
        ret

jft_alloc:
        push    es
        push    ax
        push    cx
        push    di
        mov     es,[rd_psp]
        call    jft_ptr                 ; -> ES:DI the table, CX its size
        xor     bx,bx
ja_loop:
        cmp     bx,cx
        jae     ja_full
        cmp     byte ptr es:[bx+di],0FFh
        je      ja_got
        inc     bx
        jmp     short ja_loop
ja_got:
        pop     di
        pop     cx
        pop     ax
        pop     es
        clc
        ret
ja_full:
        pop     di
        pop     cx
        pop     ax
        pop     es
        stc
        ret

;=====================================================================
; file_commit - SI = an SFT that is being closed.  Write its directory
;               entry back if anything about it changed.
;=====================================================================
file_commit:
        pusha
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jnz     fcm_done
        test    byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        jz      fcm_done

        mov     al,[si+SFT_DRIVE]
        push    si
        call    rd_getvol
        mov     di,si
        pop     si
        jc      fcm_done                ; DI = volume, SI = the SFT

        mov     eax,dword ptr [si+SFT_DIRLBA]
        push    si
        mov     si,di
        call    dir_win
        pop     si
        jc      fcm_done

        mov     bx,[si+SFT_DIROFF]
        mov     eax,dword ptr [si+SFT_SIZE]
        mov     dword ptr [rd_dirbuf + bx + DE_SIZE],eax
        mov     eax,dword ptr [si+SFT_START]
        mov     word ptr [rd_dirbuf + bx + DE_CLUSLO],ax
        shr     eax,16
        mov     word ptr [rd_dirbuf + bx + DE_CLUSHI],ax
        mov     ax,[si+SFT_TIME]
        mov     word ptr [rd_dirbuf + bx + DE_TIME],ax
        mov     ax,[si+SFT_DATE]
        mov     word ptr [rd_dirbuf + bx + DE_DATE],ax
        or      byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_ARCHIVE
        call    dir_commit
        call    rd_flush
fcm_done:
        popa
        ret

;=====================================================================
; sft_clus - SI = an SFT, EAX = a byte position -> EBX = the cluster
;            it is in.  CF if the chain ends before it.
;
; The walk starts from where the file already is when the position
; asked for is ahead of it, which is what makes reading a file
; straight through cost one step per cluster instead of one walk per
; read.
;=====================================================================
sft_clus:
        push    ecx
        push    edx
        push    edi
        push    si

        mov     al,[si+SFT_DRIVE]
        mov     di,si
        call    rd_getvol               ; -> SI = volume
        jc      sc_bad
        movzx   ecx,byte ptr [si+VOL_SPC]
        shl     ecx,9                   ; bytes per cluster
        mov     edx,eax
        xor     eax,eax
        xchg    eax,edx
        xor     edx,edx
        div     ecx                     ; EAX = which cluster of the file

        mov     ebx,dword ptr [di+SFT_CURCLUS]
        mov     edx,dword ptr [di+SFT_CURIDX]
        cmp     ebx,2
        jb      sc_fromstart
        cmp     eax,edx
        jae     sc_walk
sc_fromstart:
        mov     ebx,dword ptr [di+SFT_START]
        xor     edx,edx
        cmp     ebx,2
        jb      sc_bad
sc_walk:
        cmp     edx,eax
        je      sc_got
        push    eax
        mov     eax,ebx
        call    fat_get
        call    fat_eoc
        jc      sc_endpop
        cmp     eax,2
        jb      sc_endpop
        mov     ebx,eax
        pop     eax
        inc     edx
        jmp     short sc_walk
sc_endpop:
        pop     eax
        jmp     short sc_bad
sc_got:
        mov     dword ptr [di+SFT_CURCLUS],ebx
        mov     dword ptr [di+SFT_CURIDX],edx
        pop     si
        pop     edi
        pop     edx
        pop     ecx
        clc
        ret
sc_bad:
        pop     si
        pop     edi
        pop     edx
        pop     ecx
        stc
        ret

;--- sft_grow - SI = an SFT.  Add one cluster to the end of its chain
;--- and answer it in EBX.  CF if the volume is full.
sft_grow:
        push    eax
        push    ecx
        push    edx
        push    di
        mov     di,si
        mov     al,[si+SFT_DRIVE]
        call    rd_getvol
        jc      sg2_bad
        call    fat_alloc
        or      eax,eax
        jz      sg2_bad
        mov     ebx,eax

        mov     eax,dword ptr [di+SFT_START]
        cmp     eax,2
        jae     sg2_chain
        mov     dword ptr [di+SFT_START],ebx    ; the file had none at all
        or      byte ptr [di+SFT_FLAGS],SFF_WRITTEN
        jmp     short sg2_ok
sg2_chain:
        ; Walk to the end and hook it on
        mov     ecx,eax
sg2_walk:
        mov     eax,ecx
        call    fat_get
        call    fat_eoc
        jc      sg2_tail
        cmp     eax,2
        jb      sg2_tail
        mov     ecx,eax
        jmp     short sg2_walk
sg2_tail:
        mov     eax,ecx
        mov     edx,ebx
        call    fat_put
sg2_ok:
        mov     si,di
        pop     di
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
sg2_bad:
        mov     si,di
        pop     di
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; file_io - SI = SFT, ES:DI = buffer, ECX = bytes, [rd_iowr] = 0 read
;           / 1 write.  Answers ECX = bytes actually moved.
;
; ONE SECTOR AT A TIME THROUGH A BUFFER, always, even for a whole
; sector.  It is slower than reading straight into the caller's memory
; and it is a third of the code, with no case where the caller's
; buffer straddles a segment and no case where a partial sector needs
; a different path from a whole one.  On a machine doing one BIOS call
; per sector anyway, the copy is not what costs.
;=====================================================================
file_io:
        push    eax
        push    ebx
        push    edx
        push    bp
        mov     dword ptr [rd_iodone],0
fio_loop:
        or      ecx,ecx
        jz      fio_done

        cmp     byte ptr [rd_iowr],0
        jne     fio_wr
        ; Reading stops at the end of the file, and that is not an
        ; error - it is how a program knows where the end was.
        mov     eax,dword ptr [si+SFT_SIZE]
        sub     eax,dword ptr [si+SFT_POS]
        jbe     fio_done
        cmp     ecx,eax
        jbe     fio_wr
        mov     ecx,eax
fio_wr:
        mov     eax,dword ptr [si+SFT_POS]
        call    sft_clus                ; -> EBX
        jnc     fio_have
        cmp     byte ptr [rd_iowr],0
        je      fio_done                ; reading past the chain: stop
        ; Writing past it: extend - until the chain REACHES the
        ; position, a cluster at a time.  One cluster used to be added
        ; and the data written into it wherever the position was, so a
        ; write two clusters past the end went into the first cluster
        ; past it, and a read of that position found no cluster at all.
        ; Windows' swap file is written that way: 3MB long from its
        ; first moment, one cluster of chain (file_trunc, below).
        call    sft_grow
        jc      fio_full
        jmp     fio_wr
fio_have:
        ; Which sector of that cluster, and how far into it?
        push    si
        mov     al,[si+SFT_DRIVE]
        mov     bp,si
        call    rd_getvol
        jc      fio_poperr
        mov     eax,dword ptr [bp+SFT_POS]
        movzx   edx,byte ptr [si+VOL_SPC]
        shl     edx,9
        dec     edx
        and     eax,edx                 ; the offset within the cluster
        shr     eax,9                   ; which sector of it
        push    eax
        mov     eax,ebx
        call    clus2lba
        pop     edx
        add     eax,edx
        mov     dword ptr [rd_iolba],eax

        mov     eax,dword ptr [bp+SFT_POS]
        and     eax,1FFh                ; where in the sector
        mov     dword ptr [rd_iooff],eax

        ; WHOLE SECTORS GO STRAIGHT BETWEEN THE DISK AND THE CALLER.
        ; The note at the head of this routine was written when one
        ; BIOS call per sector was the price of everything and a copy
        ; through rd_datbuf cost nothing beside it.  With RMCACHE.INC
        ; underneath, a run of sectors is one lookup and one move, and
        ; KRNL386 loading a 40K segment is ten of those instead of
        ; eighty copies.  So a request that starts on a sector boundary
        ; with at least a sector to go takes as many whole sectors as
        ; the cluster has left, straight to or from ES:DI; what is left
        ; over - a partial sector at either end - still goes through
        ; the buffer below, as it always did.
        or      eax,eax
        jnz     fio_part
        cmp     ecx,SECSIZE
        jb      fio_part
        movzx   eax,byte ptr [si+VOL_SPC]
        sub     eax,edx                 ; sectors left in the cluster
        mov     ebx,ecx
        shr     ebx,9                   ; whole sectors asked for
        cmp     eax,ebx
        jbe     fio_run
        mov     eax,ebx
fio_run:
        cmp     eax,127
        jbe     fio_run2
        mov     eax,127
fio_run2:
        shl     eax,9
        mov     dword ptr [rd_iolen],eax
        push    ecx
        shr     eax,9
        mov     cx,ax
        mov     eax,dword ptr [rd_iolba]
        mov     bx,di                   ; ES:BX = the caller's buffer
        cmp     byte ptr [rd_iowr],0
        jne     fio_runw
        call    rd_rdsec
        jmp     short fio_rund
fio_runw:
        call    rd_wrsec
fio_rund:
        pop     ecx
        jc      fio_poperr
        pop     si                      ; the SFT again
        cmp     byte ptr [rd_iowr],0
        je      fio_written
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        jmp     fio_written
fio_part:
        mov     eax,dword ptr [rd_iooff]
        ; THE CHUNK IS CLAMPED TO THE SECTOR.  THE REQUEST IS NOT.
        ;
        ; This used to clamp ECX itself - and ECX is not the size of
        ; this chunk, it is what is left of the whole call.  So the
        ; first pass round the loop cut the request down to the tail of
        ; the first sector, moved exactly that, subtracted it from
        ; itself, reached zero and reported success.
        ;
        ; file_io has therefore never moved more than one sector in its
        ; life, and nothing noticed for five releases because nothing
        ; ever asked it for more: the boot-time reads are single
        ; sectors, and the first program EXEC loaded was 178 bytes.
        ; The first caller that wanted a second sector was the .EXE
        ; loader, which got 448 bytes of a 1184-byte program, jumped
        ; into the part that had not arrived, and took the machine with
        ; it.
        mov     edx,SECSIZE
        sub     edx,eax                 ; what is left of THIS sector
        mov     dword ptr [rd_iolen],ecx
        cmp     ecx,edx
        jbe     fio_chunk
        mov     dword ptr [rd_iolen],edx
fio_chunk:

        ; Read the sector unless the write covers all of it
        cmp     byte ptr [rd_iowr],0
        je      fio_needsec
        cmp     dword ptr [rd_iolen],SECSIZE
        je      fio_nosec
fio_needsec:
        push    es
        push    bx
        mov     eax,dword ptr [rd_iolba]
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_datbuf
        push    cx
        mov     cx,1
        call    rd_rdsec
        pop     cx
        pop     bx
        pop     es
        jc      fio_poperr
fio_nosec:
        pop     si

        ; Move the bytes
        push    ds
        push    es
        push    si
        push    di
        push    cx
        mov     ecx,dword ptr [rd_iolen]
        mov     bx,word ptr [rd_iooff]
        cmp     byte ptr [rd_iowr],0
        jne     fio_tobuf
        push    cs
        pop     ds
        mov     si,OFFSET rd_datbuf
        add     si,bx
        cld
        rep     movsb                   ; DS:SI -> ES:DI, the caller's
        jmp     short fio_moved
fio_tobuf:
        push    es
        pop     ds
        mov     si,di                   ; the caller's buffer is the source
        push    cs
        pop     es
        mov     di,OFFSET rd_datbuf
        add     di,bx
        cld
        rep     movsb
fio_moved:
        pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds

        ; Write it back if we changed it
        cmp     byte ptr [rd_iowr],0
        je      fio_written
        push    es
        push    bx
        push    si
        mov     al,[si+SFT_DRIVE]
        call    rd_getvol
        jc      fio_poperr2
        mov     eax,dword ptr [rd_iolba]
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_datbuf
        push    cx
        mov     cx,1
        call    rd_wrsec
        pop     cx
        pop     si
        pop     bx
        pop     es
        jc      fio_err
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
fio_written:
        mov     eax,dword ptr [rd_iolen]
        add     dword ptr [si+SFT_POS],eax
        add     dword ptr [rd_iodone],eax
        add     di,ax
        sub     ecx,eax

        ; A write past the old end makes the file longer
        cmp     byte ptr [rd_iowr],0
        je      fio_loop
        mov     eax,dword ptr [si+SFT_POS]
        cmp     eax,dword ptr [si+SFT_SIZE]
        jbe     fio_loop
        mov     dword ptr [si+SFT_SIZE],eax
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        jmp     fio_loop

fio_done:
        mov     ecx,dword ptr [rd_iodone]
        pop     bp
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
fio_poperr2:
        pop     si
fio_poperr:
        pop     si
fio_err:
        mov     ecx,dword ptr [rd_iodone]
        pop     bp
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret
fio_full:
        mov     ecx,dword ptr [rd_iodone]
        pop     bp
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret                             ; a short write, which is how DOS
                                        ; reports a full disk

;=====================================================================
; The calls
;=====================================================================

;--- 3Ch create / truncate; 5Bh create only if it is not there -------
fn3C:
        mov     byte ptr [rd_crnew],0
        jmp     short f3C_common
fn5B:
        mov     byte ptr [rd_crnew],1
f3C_common:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        mov     cx,[bp+F_CX]
        mov     [rd_crattr],cl
        call    sub_create
        jc      f3C_bad
        mov     [bp+F_AX],bx
        jmp     i21_ok
f3C_bad:
        jmp     i21_fail

;--- 5Ah create a uniquely named temporary file ----------------------
fn5A:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;--- 3Dh open --------------------------------------------------------
fn3D:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        mov     ax,[bp+F_AX]
        call    sub_open
        jc      f3D_bad
        mov     [bp+F_AX],bx
        jmp     i21_ok
f3D_bad:
        jmp     i21_fail

;--- 3Eh close -------------------------------------------------------
fn3E:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f3E_bad
        call    sft_release
        push    es
        push    cx
        push    di
        mov     es,[rd_psp]
        call    jft_ptr
        mov     bx,[bp+F_BX]
        mov     byte ptr es:[bx+di],0FFh
        pop     di
        pop     cx
        pop     es
        jmp     i21_ok
f3E_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 3Fh read --------------------------------------------------------
fn3F:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f3F_bad
        call    sft_isnet
        jne     f3F_ours
        mov     ax,1108h                ; a redirector's file - RMNET.INC
        jmp     net_rwfn
f3F_ours:
        movzx   ecx,word ptr [bp+F_CX]
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jnz     f3F_dev
        mov     byte ptr [rd_iowr],0
        call    file_io
        jc      f3F_ioerr
        mov     [bp+F_AX],cx
        jmp     i21_ok
f3F_dev:
        call    dev_read
        jc      f3F_deverr              ; only a loaded driver can fail
        mov     [bp+F_AX],cx
        jmp     i21_ok
f3F_deverr:
        jmp     i21_fail
f3F_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f3F_ioerr:
        mov     ax,ERR_BADBLOCK
        jmp     i21_fail

;--- 40h write.  A count of zero TRUNCATES, which is the one thing
;--- this call does that its name does not suggest.
fn40:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f40_bad
        call    sft_isnet
        jne     f40_ours
        mov     ax,1109h                ; a redirector's file - RMNET.INC
        jmp     net_rwfn
f40_ours:
        movzx   ecx,word ptr [bp+F_CX]
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jnz     f40_dev
        call    sft_touch               ; a written file is dated now
        or      ecx,ecx
        jz      f40_trunc
        mov     byte ptr [rd_iowr],1
        call    file_io
        jc      f40_ioerr
        mov     [bp+F_AX],cx
        jmp     i21_ok
f40_trunc:
        call    file_trunc
        mov     word ptr [bp+F_AX],0
        jmp     i21_ok

;--- sft_touch - SI = an SFT that is about to be written.  As DOS does:
;--- the file takes the time of its last write, unless 5701h has set
;--- one, which then stands.  The close puts it in the directory.
sft_touch:
        test    byte ptr [si+SFT_FLAGS],SFF_STAMPED
        jnz     stt_out
        push    ax
        push    dx
        call    rd_stamp
        mov     [si+SFT_TIME],ax
        mov     [si+SFT_DATE],dx
        pop     dx
        pop     ax
stt_out:
        ret
f40_dev:
        call    dev_write
        jc      f40_deverr              ; only a loaded driver can fail
        mov     [bp+F_AX],cx
        jmp     i21_ok
f40_deverr:
        jmp     i21_fail
f40_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f40_ioerr:
        mov     ax,ERR_BADBLOCK
        jmp     i21_fail

;--- file_trunc - SI = SFT.  The file ends at the current position -----
;
; AS MS-DOS's WRTEOF HAS IT: the chain is made to cover exactly the new
; length - clusters added up to it, those past it given back - and only
; then is the length set.  Until v0.63 only the length was set: a file
; made longer this way had no clusters behind its new end (Windows makes
; its swap file 3MB long so, and had one cluster of it), and a file made
; shorter kept every cluster it had, lost to everything once closed.  If
; the volume fills first the length stays as it was; the write of
; nothing reports nothing written either way.
file_trunc:
        pushad
        mov     eax,dword ptr [si+SFT_POS]
        or      eax,eax
        jz      ft_none

        dec     eax                     ; the cluster of the last byte
ft_reach:
        push    eax
        call    sft_clus                ; -> EBX
        pop     eax
        jnc     ft_have
        call    sft_grow                ; not there yet: one more
        jc      ft_done                 ; (the volume is full)
        jmp     ft_reach
ft_have:
        ; ...and nothing after it
        push    si
        mov     al,[si+SFT_DRIVE]
        call    rd_getvol               ; SI = the volume
        jc      ft_popset
        mov     eax,ebx
        call    fat_get                 ; what follows it
        call    fat_eoc
        jc      ft_popset               ; nothing: it was the last
        cmp     eax,2
        jb      ft_popset
        push    eax
        mov     eax,ebx
        mov     edx,0FFFFFFFh
        call    fat_put                 ; it is the last now
        pop     eax
        call    fat_freechain           ; and the rest go back
ft_popset:
        pop     si
        jmp     short ft_set

ft_none:
        ; Cut to nothing: the whole chain goes back
        mov     ebx,dword ptr [si+SFT_START]
        cmp     ebx,2
        jb      ft_set
        push    si
        mov     al,[si+SFT_DRIVE]
        call    rd_getvol
        jc      ft_pop
        mov     eax,ebx
        call    fat_freechain
ft_pop:
        pop     si
        mov     dword ptr [si+SFT_START],0
ft_set:
        mov     eax,dword ptr [si+SFT_POS]
        mov     dword ptr [si+SFT_SIZE],eax
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        mov     dword ptr [si+SFT_CURCLUS],0    ; walks start again from
        mov     dword ptr [si+SFT_CURIDX],0     ; the front: the one it was
ft_done:                                        ; at may be gone
        popad
        ret

;--- 42h seek --------------------------------------------------------
fn42:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f42_bad
        movzx   eax,word ptr [bp+F_CX]
        shl     eax,16
        mov     ax,[bp+F_DX]
        mov     cl,byte ptr [bp+F_AX]
        cmp     cl,0
        je      f42_set
        cmp     cl,1
        je      f42_cur
        cmp     cl,2
        je      f42_end
        mov     ax,ERR_FUNC
        jmp     i21_fail
f42_cur:
        add     eax,dword ptr [si+SFT_POS]
        jmp     short f42_set
f42_end:
        ; A REDIRECTOR'S FILE opened deny-none or deny-read is the one case
        ; where somebody else may have moved its end since it was opened,
        ; so DOS asks the redirector where the end is (1121h; DOS6SRC
        ; dos/handle.asm, $LSEEK).  Anything else is measured against the
        ; size in the entry, which is DOS's field in either kind.
        call    sft_isnet
        jne     f42_size
        mov     dl,byte ptr [si+SFT_MODE]
        and     dl,70h
        cmp     dl,40h                  ; deny none
        je      f42_net
        cmp     dl,30h                  ; deny read
        jne     f42_size
f42_net:
        mov     cx,[bp+F_CX]
        mov     dx,[bp+F_DX]
        mov     ax,1121h
        call    net_sft                 ; -> DX:AX, the new position
        jc      f42_fail
        shl     edx,16
        mov     dx,ax
        mov     eax,edx
        jmp     short f42_set
f42_size:
        add     eax,dword ptr [si+SFT_SIZE]
f42_set:
        mov     dword ptr [si+SFT_POS],eax
        mov     [bp+F_AX],ax
        shr     eax,16
        mov     [bp+F_DX],ax
        jmp     i21_ok
f42_bad:
        mov     ax,ERR_BADHANDLE
f42_fail:
        jmp     i21_fail

;--- 45h duplicate a handle ------------------------------------------
fn45:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f45_bad
        push    bx
        call    jft_alloc               ; -> BX
        jc      f45_full
        mov     cx,bx
        pop     bx
        push    es
        push    di
        mov     es,[rd_psp]
        push    cx
        call    jft_ptr                 ; -> ES:DI the table
        pop     cx
        mov     al,es:[bx+di]
        mov     bx,cx
        mov     es:[bx+di],al
        pop     di
        pop     es
        call    sft_addref
        mov     [bp+F_AX],bx
        jmp     i21_ok
f45_full:
        pop     bx
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail
f45_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 46h force one handle to be another ------------------------------
fn46:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f46_bad
        mov     cx,[bp+F_CX]
        push    es
        push    di
        mov     es,[rd_psp]
        push    cx
        call    jft_ptr                 ; CX = how many there really are
        mov     di,cx
        pop     cx
        cmp     cx,di
        pop     di
        pop     es
        jae     f46_bad

        ; If the target is open, close it first - which is exactly what
        ; makes "> file" work: handle 1 is forced onto the file, and
        ; the console it used to be goes away.
        push    bx
        mov     bx,cx
        call    sft_get
        jc      f46_free
        call    sft_release
f46_free:
        pop     bx
        push    es
        push    di
        mov     es,[rd_psp]
        push    cx
        call    jft_ptr
        pop     cx
        mov     al,es:[bx+di]
        mov     bx,cx
        mov     es:[bx+di],al
        pop     di
        pop     es
        call    sft_addref
        jmp     i21_ok
f46_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 41h delete ------------------------------------------------------
fn41:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    net_canon
        jc      f41_bad
        jnz     f41_net
        call    rd_path
        jc      f41_bad
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      f41_nofile
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jnz     f41_nofile
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_READONLY
        jnz     f41_ro

        call    rd_declus               ; -> EBX
        ; The entry at rd_deoff and no other.  Until v0.60 a line above
        ; this one also wrote E5h at rd_dirbuf + DE_NAME - the FIRST
        ; entry of the sector - so every delete took a second file with
        ; it: CONFIG.SYS, the first time anything was deleted on RMDOS
        ; and looked at afterwards.
        push    bx
        mov     bx,[rd_deoff]
        mov     byte ptr [rd_dirbuf + bx + DE_NAME],0E5h
        pop     bx
        call    dir_commit
        cmp     ebx,2
        jb      f41_nochain
        mov     eax,ebx
        call    fat_freechain
f41_nochain:
        call    rd_flush
        jmp     i21_ok
f41_ro:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f41_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f41_bad:
        jmp     i21_fail
f41_net:
        ; A redirector's: $UNLINK's search attribute, hidden and system
        ; files may go, and no wildcards (RMNET.INC)
        call    net_nometa
        jc      f41_nofile
        mov     al,ATTR_HIDDEN OR ATTR_SYSTEM
        call    net_sattr
        mov     ax,1113h
        jmp     net_simple

;--- 43h get or set attributes ---------------------------------------
fn43:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    net_canon
        jc      f43_bad
        jnz     f43_net
        call    rd_path
        jc      f43_bad
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      f43_nofile
        mov     bx,[rd_deoff]
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jnz     f43_set
        movzx   cx,byte ptr [rd_dirbuf + bx + DE_ATTR]
        mov     [bp+F_CX],cx
        jmp     i21_ok
f43_set:
        cmp     al,1
        jne     f43_fn
        mov     cl,byte ptr [bp+F_CX]
        and     cl,NOT (ATTR_DIR OR ATTR_VOLUME)
        mov     byte ptr [rd_dirbuf + bx + DE_ATTR],cl
        call    dir_commit
        call    rd_flush
        jmp     i21_ok
f43_fn:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f43_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f43_bad:
        jmp     i21_fail

        ; A REDIRECTOR'S: 110Fh answers the attributes in AX, and 110Eh
        ; is pushed the new ones, which DOS lets be only read-only,
        ; hidden, system and archive (DOS6SRC dos/finfo.asm and $CHMOD)
f43_net:
        call    net_nometa
        jc      f43_netpath
        mov     al,ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR
        call    net_sattr
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jnz     f43_netset
        mov     ax,110Fh
        call    net_pcall               ; -> AX = the attributes
        jc      f43_bad
        mov     [bp+F_CX],ax
        jmp     i21_ok
f43_netset:
        cmp     al,1
        jne     f43_fn
        mov     ax,[bp+F_CX]
        test    al,NOT (ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_ARCHIVE)
        jnz     f43_netacc
        mov     word ptr [net_parm],ax
        mov     ax,110Eh
        jmp     net_simple
f43_netacc:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f43_netpath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail

;--- 57h a file's time and date --------------------------------------
fn57:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f57_bad
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jnz     f57_set
        mov     ax,[si+SFT_TIME]
        mov     [bp+F_CX],ax
        mov     ax,[si+SFT_DATE]
        mov     [bp+F_DX],ax
        jmp     i21_ok
f57_set:
        cmp     al,1
        jne     f57_fn
        mov     ax,[bp+F_CX]
        mov     [si+SFT_TIME],ax
        mov     ax,[bp+F_DX]
        mov     [si+SFT_DATE],ax
        call    sft_isnet
        je      f57_netset
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN OR SFF_STAMPED
        jmp     i21_ok
f57_netset:
        ; A redirector's file says it in DOS's flags word, where the
        ; redirector looks at its close: not clean, and do not stamp it
        ; (DOS6SRC dos/handle.asm, $File_Times)
        and     byte ptr [si+SFT_DEVINFO],NOT NF_CLEAN AND 0FFh
        or      byte ptr [si+SFT_DEVINFO+1],NF_NODATE
        jmp     i21_ok
f57_fn:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f57_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 44h IOCTL, and only the parts that say what a handle IS ---------
; Windows asks 4400h about every handle it opens to find out whether
; it is talking to a file or to the console, and behaves differently
; for the answer.  The rest of the family is device control, and the
; only devices here that can be controlled are the drivers CONFIG.SYS
; loaded: 01h, 02h, 03h, 06h, 07h and 0Ch reach those (f44_drv*).
fn44:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jz      f44_get
        cmp     al,1
        je      f44_set
        cmp     al,2
        je      f44_ioin
        cmp     al,3
        je      f44_ioout
        cmp     al,0Ch
        je      f44_genchar
        cmp     al,6
        je      f44_instat
        cmp     al,7
        je      f44_outstat
        cmp     al,8
        je      f44_changeable
        cmp     al,9
        je      f44_remote
        cmp     al,0Ah
        je      f44_hremote
        cmp     al,0Bh
        je      f44_retry
        cmp     al,0Dh
        je      f44_generic
        cmp     al,0Eh
        je      f44_drvmap
        mov     ax,ERR_FUNC
        jmp     i21_fail
f44_get:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        mov     dx,[si+SFT_DEVINFO]     ; a device's word, or a file's
                                        ; drive - sft_setdev and
                                        ; sub_fromdir wrote it
        mov     [bp+F_DX],dx
        mov     [bp+F_AX],dx
        jmp     i21_ok
f44_set:
        ; RAW OR COOKED, for a loaded driver: the one bit DOS lets a
        ; program change, and READDEV and WRTDEV's rules turn on it.
        ; This DOS's own devices go on ignoring it, as they always have.
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        call    f44_isdrv
        jc      f44_setown
        cmp     byte ptr [bp+F_DX+1],0
        jne     f44_unsup
        and     byte ptr [si+SFT_DEVINFO],NOT DIF_RAW
        mov     al,byte ptr [bp+F_DX]
        and     al,DIF_RAW
        or      byte ptr [si+SFT_DEVINFO],al
f44_setown:
        jmp     i21_ok
f44_instat:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        mov     al,DVC_INSTAT
        call    f44_drvstat
        mov     byte ptr [bp+F_AX],al   ; FFh ready, unless a driver says
        jmp     i21_ok                  ; it is busy
f44_outstat:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        mov     al,DVC_OUTSTAT
        call    f44_drvstat
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;--- f44_isdrv - SI = an SFT -> CF clear if it is a loaded driver ----
;--- A redirector's file is not, whatever it keeps where SFF_* would be.
f44_isdrv:
        call    sft_isnet
        je      f44i_no
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jz      f44i_no
        cmp     byte ptr [si+SFT_DRIVE],DEV_DRV
        jne     f44i_no
        clc
        ret
f44i_no:
        stc
        ret

;--- f44_drvstat - SI = an SFT, AL = the status request -> AL = 00h if
;--- a loaded driver answers busy, FFh otherwise.  A driver that fails
;--- the request is not busy for the asking.
f44_drvstat:
        call    f44_isdrv
        jc      f44s_ready
        call    drv_short
        jc      f44s_ready
        test    ax,DVS_BUSY
        jz      f44s_ready
        mov     al,00h
        ret
f44s_ready:
        mov     al,0FFh
        ret

;--- 02h and 03h: IOCTL read and write, DS:DX the buffer, CX the count,
;--- AX what moved.  Only a loaded driver whose header says it takes
;--- IOCTL requests; anything else is "invalid function", as DOS says
;--- of a device without the bit.  This is the call IFSMGR.386 makes
;--- of IFSHLP.SYS (RMDEV.INC).
f44_ioin:
        mov     al,DVC_IOCTLIN
        jmp     short f44_io
f44_ioout:
        mov     al,DVC_IOCTLOUT
f44_io:
        mov     bx,[bp+F_BX]
        push    ax
        call    sft_get
        pop     ax
        jc      f44_bad
        call    f44_isdrv
        jc      f44_unsup
        test    word ptr [si+SFT_DEVINFO],DVA_IOCTL
        jz      f44_unsup
        mov     cx,[bp+F_CX]
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        call    drv_xfer
        jc      f44_drverr
        mov     [bp+F_AX],cx
        jmp     i21_ok
f44_drverr:
        call    drv_err
        jmp     i21_fail

;--- 0Ch: the generic IOCTL for a character device.  CH the category,
;--- CL the function, DS:DX the parameter block, SI and DI passed on.
;--- A loaded driver whose header has the generic IOCTL bit.
f44_genchar:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        call    f44_isdrv
        jc      f44_unsup
        call    drv_attr
        test    ax,DVA_GENIOCTL
        jz      f44_unsup
        push    fs
        push    bp
        mov     bx,bp                   ; BX = the frame, BP = the packet
        sub     sp,RQ_ROOM
        mov     bp,sp
        call    rq_zero
        mov     byte ptr [bp+RQ_LEN],RQ_GLEN
        mov     byte ptr [bp+RQ_CMD],DVC_GENIOCTL
        mov     ax,[bx+F_CX]
        mov     byte ptr [bp+RQ_GMINOR],al
        mov     byte ptr [bp+RQ_GCAT],ah
        mov     ax,[bx+F_SI]
        mov     word ptr [bp+RQ_GSI],ax
        mov     ax,[bx+F_DI]
        mov     word ptr [bp+RQ_GDI],ax
        mov     ax,[bx+F_DX]
        mov     word ptr [bp+RQ_GPKT],ax
        mov     ax,[bx+F_DS]
        mov     word ptr [bp+RQ_GPKT+2],ax
        lfs     di,dword ptr [si+SFT_DEVPTR]
        mov     ax,ss
        mov     es,ax
        push    bx
        mov     bx,bp
        call    drv_call
        pop     bx
        lea     sp,[bp+RQ_ROOM]
        pop     bp
        pop     fs
        jc      f44_drverr
        jmp     i21_ok
;--- 08h: can the media be changed?  AX = 0 removable, 1 fixed.  Out of
;--- the volume table, with no mount: a floppy drive with no disk in it
;--- must answer, not read.
f44_changeable:
        call    f44_notnet
        call    f44_vol
        jc      f44_baddrv
        mov     word ptr [bp+F_AX],1
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jz      f44_fixed
        mov     word ptr [bp+F_AX],0
f44_fixed:
        jmp     i21_ok

;--- 09h: is the drive somebody else's?  DX = the device attributes,
;--- bit 12 remote: 1000h and nothing else for a redirector's drive, as
;--- DOS answers it (Ioctl_Drive_attr in DOS6SRC dos/ioctl.asm), and 0
;--- for a drive of this DOS's own, which is still checked.
f44_remote:
        mov     al,byte ptr [bp+F_BX]
        or      al,al
        jnz     f44r_named
        mov     al,[rd_drive]
        inc     al
f44r_named:
        dec     al
        call    net_drive
        jz      f44r_ours
        mov     word ptr [bp+F_DX],1000h
        jmp     i21_ok
f44r_ours:
        call    f44_vol
        jc      f44_baddrv
        mov     word ptr [bp+F_DX],0
        jmp     i21_ok

;--- 0Ah: is the HANDLE somebody else's?  DX = DOS's flags word for it,
;--- whose bit 15 says a redirector's file - for a device or a file of
;--- this DOS's own, the same word 00h answers.
f44_hremote:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        mov     dx,[si+SFT_DEVINFO]
        mov     [bp+F_DX],dx
        jmp     i21_ok

;--- 0Bh: how hard to retry a locked region.  Accepted and forgotten.
f44_retry:
        jmp     i21_ok

;--- 0Eh: does the drive answer to another letter?  AL = 0 if it is the
;--- only one; otherwise the letter that has the drive now, 1 = A.
;--- A MACHINE WITH ONE DISKETTE DRIVE HAS IT AS A: AND B:, and MS-DOS
;--- says so here: 1 for both, A: having it.  Windows 3.1 asks, and
;--- takes a B: whose answer is not its own letter for no drive at all
;--- - the kernel gives B: a slot of its own whatever the machine has
;--- (FAT.INC, disk_scan), so RMDOS answered 0 and File Manager showed
;--- a second diskette drive.  How many there are is the BIOS's
;--- equipment word, which is where MS-DOS learns it too.
f44_drvmap:
        call    f44_notnet
        call    f44_vol                 ; -> SI, AL = the drive (0 = A)
        jc      f44_baddrv
        mov     byte ptr [bp+F_AX],0
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jz      f44m_out
        cmp     al,1
        ja      f44m_out                ; past B: is nobody's alias
        int     11h                     ; AX = the equipment word
        test    al,01h                  ; no diskette drives at all, or
        jz      f44m_out
        test    al,0C0h                 ; two or more (bits 6-7 = the count
        jnz     f44m_out                ; less one): each has its letter
        mov     byte ptr [bp+F_AX],1    ; one drive, two letters, A:'s
f44m_out:
        jmp     i21_ok

;--- 0Dh: the generic block device request.  CH = 08h is the disk
;--- category, and CL = 60h, get device parameters, is the one that
;--- is asked: DS:DX -> the block, special byte, device type, the
;--- attributes, the cylinders, the media type and then the BPB as
;--- the volume has it.  The volume is mounted for it, as DOS reads the
;--- disk for it too.  Everything else in the category is refused.
;---
;--- CALMIRA ASKS ALL OF THESE, 08h, 09h, 0Eh and 0Dh/60h, for every
;--- letter while it starts, and put up an "Error" form for each of
;--- the thirty-odd refusals (2026-10-05, the enhanced-mode sweep).  On
;--- MS-DOS 5 it starts clean.
f44_generic:
        cmp     byte ptr [bp+F_CX+1],08h
        jne     f44_unsup
        cmp     byte ptr [bp+F_CX],60h
        jne     f44_unsup
        mov     al,byte ptr [bp+F_BX]
        or      al,al
        jnz     f44g_named
        mov     al,[rd_drive]
        inc     al
f44g_named:
        dec     al
        ; A FLOPPY DRIVE IS NOT READ FOR THIS.  DOS answers 60h out of
        ; the driver's default table for a diskette drive, and so does
        ; this: a 1.44M diskette's BPB, whether or not there is a disk
        ; in the drive.  Mounting it here would read it, and an empty
        ; drive under Windows is a critical error nobody can answer -
        ; Calmira asks about A: while it starts, and did stop there.
        ; A hard disk is mounted (once, at the first use) and answers
        ; with its own BPB.
        push    ax
        call    f44_vol                 ; -> SI, no mount
        pop     ax
        jc      f44_baddrv
        test    byte ptr [si+VOL_FLAGS],VF_FLOPPY
        jnz     f44g_floppy
        call    rd_getvol               ; a hard disk: mounted, the BPB real
        jc      f44_baddrv
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        mov     byte ptr es:[di+1],5    ; a fixed disk...
        mov     word ptr es:[di+2],1    ; ...which cannot be changed
        jmp     short f44g_type
f44g_floppy:
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        mov     byte ptr es:[di+1],7    ; a 1.44M diskette drive
        mov     word ptr es:[di+2],0    ; changeable
        mov     word ptr es:[di+4],80   ; cylinders
        mov     byte ptr es:[di+6],0    ; media type: 1.44M
        mov     word ptr es:[di+7],SECSIZE
        mov     byte ptr es:[di+9],1    ; sectors per cluster
        mov     word ptr es:[di+0Ah],1  ; reserved
        mov     byte ptr es:[di+0Ch],2  ; FATs
        mov     word ptr es:[di+0Dh],224
        mov     word ptr es:[di+0Fh],2880
        mov     byte ptr es:[di+11h],0F0h
        mov     word ptr es:[di+12h],9  ; sectors per FAT
        mov     word ptr es:[di+14h],18 ; per track
        mov     word ptr es:[di+16h],2  ; heads
        mov     dword ptr es:[di+18h],0 ; hidden
        mov     dword ptr es:[di+1Ch],0 ; the 32-bit total: none
        jmp     i21_ok
f44g_type:
        movzx   eax,word ptr [si+VOL_SPT]
        movzx   ebx,word ptr [si+VOL_HEADS]
        mul     ebx                     ; EAX = sectors per cylinder
        mov     ebx,eax
        xor     ecx,ecx                 ; cylinders, 0 if there is no geometry
        or      ebx,ebx
        jz      f44g_cyl
        mov     eax,dword ptr [si+VOL_TOTSEC]
        xor     edx,edx
        div     ebx
        mov     ecx,eax
        cmp     ecx,0FFFFh
        jbe     f44g_cyl
        mov     ecx,0FFFFh
f44g_cyl:
        mov     es:[di+4],cx            ; cylinders
        mov     byte ptr es:[di+6],0    ; media type: other
        mov     word ptr es:[di+7],SECSIZE
        mov     al,[si+VOL_SPC]
        mov     es:[di+9],al
        mov     ax,[si+VOL_RSVD]
        mov     es:[di+0Ah],ax
        mov     al,[si+VOL_NFATS]
        mov     es:[di+0Ch],al
        mov     ax,[si+VOL_ROOTENT]
        mov     es:[di+0Dh],ax
        mov     eax,dword ptr [si+VOL_TOTSEC]
        xor     cx,cx                   ; the 16-bit count, when it fits
        cmp     eax,10000h
        jae     f44g_big
        mov     cx,ax
        xor     eax,eax                 ; ...and then the 32-bit one is 0
f44g_big:
        mov     es:[di+0Fh],cx
        mov     dword ptr es:[di+1Ch],eax
        mov     al,[si+VOL_MEDIA]
        mov     es:[di+11h],al
        mov     eax,dword ptr [si+VOL_SPF]
        cmp     byte ptr [si+VOL_FATTYPE],32
        jne     f44g_spf
        xor     eax,eax                 ; FAT32 says 0 here
f44g_spf:
        mov     es:[di+12h],ax
        mov     ax,[si+VOL_SPT]
        mov     es:[di+14h],ax
        mov     ax,[si+VOL_HEADS]
        mov     es:[di+16h],ax
        mov     eax,dword ptr [si+VOL_START]
        mov     dword ptr es:[di+18h],eax       ; hidden: the sectors before it
        jmp     i21_ok

;--- f44_notnet - BL = the drive (0 = default, 1 = A:).  Returns if it is
;--- not a redirector's; if it is, the call ends "invalid function", as
;--- MS-DOS ends 08h and 0Eh for one (Check_If_Net in DOS6SRC
;--- dos/ioctl.asm): the media is not this DOS's to describe.  Called,
;--- so the return address is thrown away on that way out.
f44_notnet:
        mov     al,byte ptr [bp+F_BX]
        or      al,al
        jnz     f44n_named
        mov     al,[rd_drive]
        inc     al
f44n_named:
        dec     al
        call    net_drive
        jnz     f44n_net
        ret
f44n_net:
        pop     ax                      ; our return address
        jmp     f44_unsup

;--- f44_vol - BL = the drive (0 = default, 1 = A:) -> SI = its row in
;--- the volume table, CF if there is no such drive.  The table only:
;--- no mount, no read.
f44_vol:
        mov     al,byte ptr [bp+F_BX]
        or      al,al
        jnz     f44v_named
        mov     al,[rd_drive]
        inc     al
f44v_named:
        dec     al
        cmp     al,LASTDRIVE
        jae     f44v_none
        push    ax
        movzx   si,al
        mov     ax,VOL_LEN
        mul     si
        mov     si,ax
        pop     ax
        add     si,OFFSET rd_vols
        test    byte ptr [si+VOL_FLAGS],VF_PRESENT
        jz      f44v_none
        clc
        ret
f44v_none:
        stc
        ret

f44_baddrv:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail
f44_unsup:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f44_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 39h, 3Ah, 3Bh - directories -------------------------------------
fn3B:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    net_canon
        jc      f3B_bad
        jnz     f3B_net
        call    rd_path
        jc      f3B_bad
        ; rd_path stopped at the last component, so look it up and
        ; stand there.  An empty last component means the caller named
        ; a directory with a trailing separator and is already there.
        cmp     byte ptr [rd_fcb],' '
        je      f3B_here
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        push    ax
        call    dir_find
        pop     ax
        jc      f3B_nopath
        push    bx
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        pop     bx
        jz      f3B_nopath
        call    rd_declus               ; -> EBX
f3B_here:
        ; THE PATH TEXT GOES IN WITH THE CLUSTER.  Until v0.60 only the
        ; cluster did, so 47h answered the root after every CD and a
        ; prompt of $P never moved; and since relative paths now start
        ; from this text (rd_canon), it is the directory, not a label.
        ; rd_canbuf still holds what rd_path walked: "D:\" and the rest.
        mov     si,OFFSET rd_canbuf + 3
        mov     cx,64
f3B_len:
        cmp     byte ptr [si],0
        je      f3B_fits
        inc     si
        loop    f3B_len
        jmp     short f3B_nopath        ; longer than a DOS path may be
f3B_fits:
        mov     al,byte ptr [rp_drv]
        movzx   di,al
        mov     ax,CDS_LEN
        mul     di
        mov     di,ax
        add     di,RL_CDS
        push    es
        mov     es,[rd_lowseg]          ; the CDS is in the block below 64K
        mov     dword ptr es:[di+CDS_CLUS],ebx
        add     di,CDS_PATH
        mov     si,OFFSET rd_canbuf + 3
        cld
f3B_copy:
        lodsb
        stosb
        or      al,al
        jnz     f3B_copy
        pop     es
        jmp     i21_ok
f3B_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail
f3B_bad:
        jmp     i21_fail

f3B_net:
        jmp     net_chdir               ; a redirector's drive - RMNET.INC

;--- 47h where am I?  DL = drive, DS:SI -> 64 bytes ------------------
; The text path is kept alongside the cluster because there is no way
; to reconstruct it: a directory knows its parent through "..", but
; not its own name, so walking up gives the shape of the path and none
; of the words in it.
fn47:
        mov     al,byte ptr [bp+F_DX]
        or      al,al
        jnz     f47_named
        mov     al,[rd_drive]
        inc     al
f47_named:
        dec     al
        call    cds_inuse               ; a letter in use, ours or a
        jz      f47_bad                 ; redirector's
        push    es
        movzx   si,al
        mov     ax,CDS_LEN
        mul     si
        mov     si,ax
        add     si,RL_CDS
        mov     es,[bp+F_DS]
        mov     di,[bp+F_SI]
        mov     cx,64
        push    ds
        mov     ds,word ptr cs:[rd_lowseg]      ; the CDS is in the block
        ; FROM THE END OF THE DRIVE'S OWN ROOT, as $CURRENT_DIR takes it
        ; (curdir_end): "C:\" ends at 2, and a redirector's root can be
        ; longer - "\\SERVER\SHARE" - and is not part of the answer
        mov     ax,word ptr [si+CDS_BACK]
        cmp     ax,64
        jbe     f47_root
        mov     ax,2
f47_root:
        add     si,ax
        cmp     byte ptr [si],'\'
        jne     f47_copy
        inc     si
f47_copy:
        cld                                     ; below 64K
f47_byte:
        lodsb                                   ; up to its NUL, and no
        stosb                                   ; further into the
        or      al,al                           ; structure
        loopnz  f47_byte
        mov     byte ptr es:[di-1],0            ; 64 without one: ended
        pop     ds
        pop     es
        mov     word ptr [bp+F_AX],0100h
        jmp     i21_ok
f47_bad:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;--- 4Eh find first, 4Fh find next -----------------------------------
fn4E:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    net_canon
        jc      f4E_bad
        jnz     f4E_net
        call    rd_path
        jc      f4E_bad
        mov     [rd_ffdrv],al           ; which drive - the DTA records it,
                                        ; and 4Fh finds the volume by it
        mov     [rd_ffvol],si
        mov     dword ptr [rd_ffdir],ebx
        mov     al,byte ptr [bp+F_CX]
        mov     [rd_ffattr],al

        push    cs
        pop     es
        mov     si,OFFSET rd_fcb
        mov     di,OFFSET rd_ffname
        mov     cx,11
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds

        mov     si,[rd_ffvol]
        mov     eax,dword ptr [rd_ffdir]
        call    dir_first
        jc      f4E_none
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_ffsec],eax
        mov     eax,dword ptr [rd_dfclus]
        mov     dword ptr [rd_ffclus],eax
        mov     eax,dword ptr [rd_dfleft]
        mov     dword ptr [rd_ffleft],eax
        mov     al,[rd_dfroot]
        mov     [rd_ffroot],al
        mov     word ptr [rd_ffent],0
        jmp     short f4F_scan
f4E_none:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f4E_bad:
        jmp     i21_fail
f4E_net:
        mov     al,byte ptr [bp+F_CX]   ; a redirector's drive: the search
        call    net_sattr               ; attribute is the caller's CL
        mov     ax,111Bh
        jmp     net_find

fn4F:
        ; A SEARCH A REDIRECTOR STARTED says so in the first of its
        ; twenty-one bytes, bit 7, which no drive number of this DOS's
        ; own has; DOS gives the twenty-one back to it in SEARCHBUF.
        push    es
        mov     es,[rd_dtaseg]
        mov     di,[rd_dtaoff]
        test    byte ptr es:[di],80h
        pop     es
        jz      f4F_ours
        jmp     net_findnext            ; RMNET.INC
f4F_ours:
        ; THE SEARCH IS IN THE CALLER'S DTA, not in these globals - see
        ; ff_seek for why that distinction is the whole of this bug.
        call    ff_seek
        jc      f4F_none
f4F_scan:
        mov     si,[rd_ffvol]
        mov     eax,dword ptr [rd_ffsec]
        mov     dword ptr [rd_dfsec],eax
        mov     eax,dword ptr [rd_ffclus]
        mov     dword ptr [rd_dfclus],eax
        mov     eax,dword ptr [rd_ffleft]
        mov     dword ptr [rd_dfleft],eax
        mov     al,[rd_ffroot]
        mov     [rd_dfroot],al
f4F_sector:
        mov     eax,dword ptr [rd_dfsec]
        call    dir_win
        jc      f4F_none
        mov     bx,[rd_ffent]
f4F_entry:
        cmp     bx,SECSIZE
        jae     f4F_nextsec
        mov     al,byte ptr [rd_dirbuf + bx]
        or      al,al
        jz      f4F_none
        cmp     al,0E5h
        je      f4F_next
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        and     al,0Fh
        cmp     al,0Fh
        je      f4F_next
        push    bx
        call    ff_match
        pop     bx
        jc      f4F_next
        jmp     short f4F_found
f4F_next:
        add     bx,DE_LEN
        jmp     short f4F_entry
f4F_nextsec:
        mov     word ptr [rd_ffent],0
        call    dir_next
        jc      f4F_none
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_ffsec],eax
        mov     eax,dword ptr [rd_dfclus]
        mov     dword ptr [rd_ffclus],eax
        mov     eax,dword ptr [rd_dfleft]
        mov     dword ptr [rd_ffleft],eax
        jmp     short f4F_sector
f4F_found:
        add     bx,DE_LEN
        mov     [rd_ffent],bx
        sub     bx,DE_LEN
        call    ff_fill
        jmp     i21_ok
f4F_none:
        mov     word ptr [rd_ffvol],0
        mov     ax,ERR_NOFILE
        jmp     i21_fail

;--- ff_match - BX = an entry.  CF set if it does not match the search
ff_match:
        push    si
        push    di
        push    cx
        ; the attribute rule: a plain search never returns a directory,
        ; a hidden file or a system file, but a search that asks for
        ; any of those gets ordinary files as well
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        mov     ah,[rd_ffattr]
        not     ah
        and     al,ATTR_DIR OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_VOLUME
        and     al,ah
        jnz     ffm_no

        lea     si,[rd_dirbuf + bx]
        mov     di,OFFSET rd_ffname
        mov     cx,11
ffm_cmp:
        mov     al,[di]
        cmp     al,'?'
        je      ffm_next
        cmp     al,[si]
        jne     ffm_no
ffm_next:
        inc     si
        inc     di
        loop    ffm_cmp
        pop     cx
        pop     di
        pop     si
        clc
        ret
ffm_no:
        pop     cx
        pop     di
        pop     si
        stc
        ret

;=====================================================================
; ff_seek - pick a search up again out of the caller's DTA.
;
; DOS keeps a search going in the twenty-one bytes in front of the
; result, and that is not an implementation detail programs are
; unaware of: it is the ONLY reason a program can have two searches
; open at once.  Windows' File Manager reads a directory tree with
; exactly that - ReadDirLevel enumerates a directory, recurses into
; each subdirectory it finds, and then carries on with the outer
; FindNext - and a DOS that keeps the search in one set of globals
; loses the outer walk the moment the inner one starts.
;
; RMDOS did.  ff_fill had written those bytes since the day it was
; written, and its comment promised 4Fh would carry on from them; 4Fh
; read the globals instead, so the promise was never kept and the
; bytes were decoration.  RMFIND.COM's fourth check is that promise
; made into a test.
;
; The layout is DOS's where DOS's is fixed - the drive at 00h, the
; eleven-byte template at 01h, the search attribute at 0Ch - and this
; kernel's where it has to be, because RMDOS walks a directory by
; absolute sector and wants a 32-bit cluster for FAT32:
;
;   0Dh word   the entry's offset in its sector
;   0Fh dword  the cluster the walk is standing in (0 = a fixed root)
;   13h word   which sector of that cluster
;
; Everything else - the absolute LBA, the sectors left in the cluster,
; whether this is a fixed root - is DERIVED by walking dir_first
; again, so there is no second copy of it that can go stale.  That
; also keeps the state inside DOS's twenty-one bytes, which the byte
; at 15h is the end of: the old code wrote a dword to 12h and put its
; top byte where the found file's attribute goes, and got away with it
; only because the attribute was written afterwards.
;
; CF set if the DTA does not hold a search this DOS can resume.
;=====================================================================
ff_seek:
        push    es
        push    ds
        pusha

        mov     es,[rd_dtaseg]
        mov     di,[rd_dtaoff]

        mov     al,es:[di]
        mov     [rd_ffdrv],al
        call    rd_getvol               ; -> SI, and CF for a drive that
        jc      ffs_bad                 ; has gone away since 4Eh
        mov     [rd_ffvol],si

        mov     al,es:[di+0Ch]
        mov     [rd_ffattr],al
        mov     ax,es:[di+0Dh]
        mov     [rd_ffent],ax

        ; the template, out of the caller's segment and into ours
        mov     bx,es
        mov     si,di
        inc     si
        push    cs
        pop     es
        mov     di,OFFSET rd_ffname
        mov     ds,bx                   ; DS:SI theirs, ES:DI ours
        mov     cx,11
        cld
        rep     movsb
        push    cs
        pop     ds

        ; and where the walk had got to
        mov     es,bx
        mov     di,[rd_dtaoff]
        mov     eax,es:[di+0Fh]
        mov     cx,es:[di+13h]

        mov     si,[rd_ffvol]
        push    cx
        call    dir_first               ; the first sector of that cluster
        pop     cx
        jc      ffs_bad
        movzx   eax,cx                  ; ...and CX sectors along it
        add     dword ptr [rd_dfsec],eax
        sub     dword ptr [rd_dfleft],eax
        jbe     ffs_bad                 ; an index past the end of the unit

        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_ffsec],eax
        mov     eax,dword ptr [rd_dfclus]
        mov     dword ptr [rd_ffclus],eax
        mov     eax,dword ptr [rd_dfleft]
        mov     dword ptr [rd_ffleft],eax
        mov     al,[rd_dfroot]
        mov     [rd_ffroot],al
        popa
        pop     ds
        pop     es
        clc
        ret
ffs_bad:
        popa
        pop     ds
        pop     es
        stc
        ret

;--- ff_secidx - AX = which sector of the current cluster, or of the
;--- fixed root, the walk is standing on.  Derived rather than tracked:
;--- dir_first sets the count for the whole unit and dir_next counts it
;--- down, so what is left says how far in we are.
ff_secidx:
        push    esi
        push    ebx
        mov     si,[rd_ffvol]
        cmp     byte ptr [rd_ffroot],0
        je      ffx_cluster
        movzx   eax,word ptr [si+VOL_ROOTENT]
        shl     eax,5
        add     eax,SECSIZE - 1
        shr     eax,9
        jmp     short ffx_left
ffx_cluster:
        movzx   eax,byte ptr [si+VOL_SPC]
ffx_left:
        sub     eax,dword ptr [rd_ffleft]
        pop     ebx
        pop     esi
        ret

;--- ff_fill - BX = the entry.  Write the caller's DTA ---------------
ff_fill:
        pusha
        push    es
        mov     es,[rd_dtaseg]
        mov     di,[rd_dtaoff]

        ; The first 21 bytes are DOS's own place to keep the search
        ; going, and ff_seek above is the code that carries on from
        ; them.  They end AT 15h, where the found file starts.
        push    bx
        push    di
        mov     al,[rd_ffdrv]
        mov     es:[di],al
        inc     di
        mov     si,OFFSET rd_ffname
        mov     cx,11
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds
        mov     al,[rd_ffattr]
        mov     es:[di],al              ; 0Ch
        inc     di
        mov     ax,[rd_ffent]
        mov     es:[di],ax              ; 0Dh
        add     di,2
        mov     eax,dword ptr [rd_ffclus]
        mov     es:[di],eax             ; 0Fh
        add     di,4
        call    ff_secidx
        mov     es:[di],ax              ; 13h, and 15h is the next byte
        pop     di
        pop     bx

        add     di,15h
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        mov     es:[di],al
        mov     ax,word ptr [rd_dirbuf + bx + DE_TIME]
        mov     es:[di+1],ax
        mov     ax,word ptr [rd_dirbuf + bx + DE_DATE]
        mov     es:[di+3],ax
        mov     eax,dword ptr [rd_dirbuf + bx + DE_SIZE]
        mov     es:[di+5],eax

        ; and the name, as text with a dot in it again
        add     di,9
        lea     si,[rd_dirbuf + bx]
        mov     cx,8
ffl_base:
        mov     al,[si]
        cmp     al,' '
        je      ffl_baseend
        mov     es:[di],al
        inc     di
        inc     si
        loop    ffl_base
ffl_baseend:
        lea     si,[rd_dirbuf + bx]
        add     si,8
        cmp     byte ptr [si],' '
        je      ffl_noext
        mov     byte ptr es:[di],'.'
        inc     di
        mov     cx,3
ffl_ext:
        mov     al,[si]
        cmp     al,' '
        je      ffl_noext
        mov     es:[di],al
        inc     di
        inc     si
        loop    ffl_ext
ffl_noext:
        mov     byte ptr es:[di],0
        pop     es
        popa
        ret

;--- 56h rename ------------------------------------------------------
;--- 5Ch lock a region.  There is no sharing here, so every lock
;--- succeeds and every unlock succeeds, which is what a DOS without
;--- SHARE loaded has always done.
fn5C:
        jmp     i21_ok

;--- 67h how many handles --------------------------------------------
;
; DOS keeps the first twenty in the PSP and MOVES THE TABLE into a
; block of its own when a program wants more, leaving PSP:32h saying
; how many and PSP:34h saying where.  This used to answer "no" to
; anything past twenty, on the grounds that the table cannot grow past
; the PSP - which is true and is not the point: it is not supposed to
; grow there.
;
; Windows' KERNEL asks at start-up.  Refused, it ran on twenty
; handles, and Program Manager's own PROGMAN.INI was the nineteenth
; file it wanted open: twenty-four opens in a row, every one refused
; with "too many open files", and a Program Manager with no groups in
; it.  See jft_ptr above, and NOTES.TXT v0.55.
;
; The new table is stamped as the PROCESS's, so terminating frees it;
; the old one goes back unless it was the PSP itself, which is never
; freed on its own.
fn67:
        mov     ax,[bp+F_BX]
IFDEF DBG_LPT
        push    bx
        mov     bx,ax
        call    rdbg_h67                ; somebody asked - which is the
        pop     bx                      ; thing nobody could see
ENDIF
        or      ax,ax
        jz      f67_ok                  ; nothing asked for is nothing to do
        cmp     ax,JFT_COUNT
        jbe     f67_ok                  ; what is already there is enough
        push    es
        push    ds
        push    si
        push    di
        push    cx
        push    dx
        push    bx
        mov     [rd_jftwant],ax

;--- where the table is now, and how much of it is in use
        mov     es,[rd_psp]
        call    jft_ptr                 ; -> ES:DI, CX
        mov     [rd_jftold],cx
        mov     [rd_jftoff],di
        mov     ax,es
        mov     [rd_jftseg],ax

;--- a block big enough for the new one
        mov     bx,[rd_jftwant]
        add     bx,15
        shr     bx,4
        call    mem_alloc               ; -> AX = segment
        jc      f67_fail
        mov     [rd_jftnew],ax
        mov     bx,ax                   ; and it belongs to the process,
        dec     bx                      ; so that terminating frees it
        mov     es,bx
        mov     bx,[rd_psp]
        mov     word ptr es:[RMCB_OWN],bx

;--- every entry closed...
        mov     es,[rd_jftnew]
        xor     di,di
        mov     cx,[rd_jftwant]
        mov     al,0FFh
        cld
        rep     stosb

;--- ...and then what was already open, copied in
        mov     si,[rd_jftoff]
        mov     cx,[rd_jftold]
        mov     es,[rd_jftnew]
        xor     di,di
        mov     ds,[rd_jftseg]          ; last, because everything above
        cld                             ; reads through DS
        rep     movsb
        push    cs
        pop     ds

;--- the PSP names the new one
        mov     es,[rd_psp]
        mov     ax,[rd_jftwant]
        mov     word ptr es:[32h],ax
        mov     word ptr es:[34h],0
        mov     ax,[rd_jftnew]
        mov     word ptr es:[36h],ax

;--- and the old one goes back, unless it was the PSP itself
        mov     ax,[rd_jftseg]
        cmp     ax,[rd_psp]
        je      f67_done
        dec     ax
        mov     es,ax
        mov     word ptr es:[RMCB_OWN],0
f67_done:
        pop     bx
        pop     dx
        pop     cx
        pop     di
        pop     si
        pop     ds
        pop     es
f67_ok:
        jmp     i21_ok
f67_fail:
        push    cs
        pop     ds
        pop     bx
        pop     dx
        pop     cx
        pop     di
        pop     si
        pop     ds
        pop     es
        mov     ax,ERR_NOMEM
        jmp     i21_fail

rd_jftwant  dw  0                       ; 67h: how many were asked for
rd_jftold   dw  0                       ; ...how many there are now
rd_jftoff   dw  0                       ; ...and where they are
rd_jftseg   dw  0
rd_jftnew   dw  0                       ; ...and where they are going

;--- 68h / 6Ah commit a file: everything of ours goes out ------------
fn68:
        call    rd_flush
        jmp     i21_ok

;--- 6Ch the extended open, which says WHICH of create and open
;--- happened - the one thing 3Ch and 3Dh cannot say between them.
fn6C:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; The subroutine forms, for EXEC and anything else inside RMDOS
;=====================================================================

;--- sub_open - ES:SI -> name, AX = mode -> BX = handle.  CF, AX = why
;=====================================================================
; sub_open - ES:SI a name, AX the mode -> BX a handle.
;
; A DEVICE IS A NAME BEFORE IT IS A FILE, and this went straight to
; the disk.  rd_sftinit hands every process five handles that already
; point at CON, AUX and PRN, and every one of the device paths below -
; dev_read, dev_write, the 44h that reports a character device - has
; worked since v0.45, so the machine looked like it had devices.  It
; could not OPEN one: "CON" is not in any directory, dir_find said so,
; and 3Dh returned "file not found".
;
; That is what stopped Windows.  KRNL286 opens CON five times as it
; starts, gets ERROR_FILE_NOT_FOUND five times, tries again with a
; different access mode, gets it five more times, and gives up with
; "Incorrect MS-DOS version.  MS-DOS 3.1 or greater required." - which
; is a message about the DOS being unusable and not about its version,
; and which sent this release looking at AH=30h, where the answer had
; been 6.22 all along.
;
; Nothing had asked before.  A program that inherits its handles never
; opens CON by name, and every test here inherits them.
;=====================================================================
sub_open:
        push    cx
        push    dx
        push    di
        push    si
        push    es
        mov     [rd_opmode],ax
        call    dev_name                ; CON, AUX, PRN, NUL, LPTn, COMn,
        jc      so_notdev               ; or a driver CONFIG.SYS loaded?
        mov     word ptr [rd_opfail],0
        call    sub_devopen             ; AL = which one
        jnc     so_good
        cmp     word ptr [rd_opfail],0  ; the driver said no, and why
        je      so_nohandles
        mov     ax,[rd_opfail]
        jmp     short so_out
so_notdev:
        call    net_canon
        jc      so_bad
        jnz     so_net
        call    rd_path
        jc      so_bad
        mov     [rd_opvol],si
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      so_nofile
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jnz     so_nofile
        call    sub_fromdir
        jc      so_nohandles
so_good:
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        clc
        ret
so_nofile:
        mov     ax,ERR_NOFILE
        jmp     short so_out
so_nohandles:
IFDEF DBG_LPT
        call    rdbg_jfail              ; whose table was it, and how full
ENDIF
        mov     ax,ERR_NOHANDLES
        jmp     short so_out
so_bad:
        mov     ax,ERR_NOPATH
so_out:
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        stc
        ret
so_net:
        ; A REDIRECTOR'S DRIVE: DOS_Open's 1116h, pushed the mode without
        ; its no-inherit bit, with $Open's search attribute - and no
        ; wildcard in the name (RMNET.INC, net_open)
        call    net_nometa
        jc      so_nofile
        mov     al,ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR
        call    net_sattr
        mov     ax,[rd_opmode]
        and     ax,007Fh
        mov     word ptr [net_parm],ax
        mov     ax,1116h
        call    net_open                ; -> BX = the handle
        jc      so_out
        jmp     so_good

;--- sub_fromdir - the entry found in rd_dirbuf at rd_deoff becomes an
;--- open file.  -> BX = the handle, SI = its SFT.  CF: SI unchanged.
;
; SI COMES BACK AS THE SFT, and until v0.60 it came back as whatever the
; caller had in it - the VOLUME RECORD, in sub_create, which then went on
; to use it as the SFT it had just made.  A create set SFF_WRITTEN in
; [volume + 5], the top byte of VOL_START, so the next thing to add
; VOL_START to a sector number (69h, INT 25h/26h, a floppy's re-mount)
; read 32MB past it.  A create that TRUNCATED zeroed VOL_FATLBA through
; SFT_POS, then had file_trunc free a chain named by VOL_SPF on the
; drive named by VOL_SPC - and never truncated the file.
sub_fromdir:
        push    si
        push    di
        push    ax
        call    jft_alloc               ; -> BX
        jc      sfd_bad
        mov     [rd_ophnd],bx
        call    sft_alloc               ; -> SI, AL = index
        jc      sfd_bad
        mov     [rd_opidx],al
        mov     di,[rd_deoff]

        mov     al,[rp_drv]
        mov     [si+SFT_DRIVE],al
        mov     byte ptr [si+SFT_DEVINFO],al    ; a file: its drive, and
        mov     byte ptr [si+SFT_DEVINFO+1],0   ; no device bit
        mov     dword ptr [si+SFT_DEVPTR],0FFFFFFFFh
        mov     ax,[rd_opmode]
        mov     [si+SFT_MODE],ax
        mov     al,byte ptr [rd_dirbuf + di + DE_ATTR]
        mov     [si+SFT_ATTR],al
        mov     byte ptr [si+SFT_FLAGS],0
        mov     eax,dword ptr [rd_dirbuf + di + DE_SIZE]
        mov     dword ptr [si+SFT_SIZE],eax
        mov     dword ptr [si+SFT_POS],0
        movzx   eax,word ptr [rd_dirbuf + di + DE_CLUSHI]
        shl     eax,16
        mov     ax,word ptr [rd_dirbuf + di + DE_CLUSLO]
        mov     dword ptr [si+SFT_START],eax
        mov     [si+SFT_CLUSLO],ax
        mov     dword ptr [si+SFT_CURCLUS],0
        mov     dword ptr [si+SFT_CURIDX],0
        mov     ax,word ptr [rd_dirbuf + di + DE_TIME]
        mov     [si+SFT_TIME],ax
        mov     ax,word ptr [rd_dirbuf + di + DE_DATE]
        mov     [si+SFT_DATE],ax
        mov     eax,dword ptr [rd_delba]
        mov     dword ptr [si+SFT_DIRLBA],eax
        mov     ax,[rd_deoff]
        mov     [si+SFT_DIROFF],ax

        push    es
        push    cx
        push    di
        mov     es,[rd_psp]
        call    jft_ptr
        mov     bx,[rd_ophnd]
        mov     al,[rd_opidx]
        mov     es:[bx+di],al
        pop     di
        pop     cx
        pop     es
        pop     ax
        pop     di
        add     sp,2                    ; not SI: it is the answer now
        clc
        ret
sfd_bad:
        pop     ax
        pop     di
        pop     si
        stc
        ret

;--- sub_create - ES:SI -> name, [rd_crattr] = attributes ------------
sub_create:
        push    cx
        push    dx
        push    di
        push    si
        push    es
        call    net_canon
        jc      sc2_bad
        jnz     sc2_net
        call    rd_path
        jc      sc2_bad
        mov     [rd_opvol],si
        mov     dword ptr [rd_crdir],ebx

        ; Is it already there?  Then this is a truncate, unless the
        ; caller asked for 5Bh, in which case it is a refusal.
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      sc2_new
        cmp     byte ptr [rd_crnew],0
        jne     sc2_exists
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jnz     sc2_exists
        mov     word ptr [rd_opmode],2
        call    sub_fromdir
        jc      sc2_nohandles
        mov     dword ptr [si+SFT_POS],0
        call    file_trunc
        call    sft_touch               ; truncated is written
        jmp     sc2_ok

sc2_new:
        mov     si,[rd_opvol]
        mov     eax,dword ptr [rd_crdir]
        call    dir_alloc
        jc      sc2_full

        ; Fill the new entry in
        mov     bx,[rd_deoff]
        push    di
        lea     di,[rd_dirbuf + bx]
        push    es
        push    cs
        pop     es
        mov     si,OFFSET rd_fcb
        mov     cx,11
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds
        pop     es
        pop     di
        mov     al,[rd_crattr]
        and     al,NOT (ATTR_DIR OR ATTR_VOLUME)
        or      al,ATTR_ARCHIVE
        mov     byte ptr [rd_dirbuf + bx + DE_ATTR],al
        mov     dword ptr [rd_dirbuf + bx + DE_SIZE],0
        mov     word ptr [rd_dirbuf + bx + DE_CLUSLO],0
        mov     word ptr [rd_dirbuf + bx + DE_CLUSHI],0
        call    rd_stamp                ; -> AX = time, DX = date
        mov     word ptr [rd_dirbuf + bx + DE_TIME],ax
        mov     word ptr [rd_dirbuf + bx + DE_DATE],dx
        mov     cx,10
        push    di
        lea     di,[rd_dirbuf + bx + 0Ch]
        xor     al,al
        push    es
        push    cs
        pop     es
        cld
        rep     stosb
        pop     es
        pop     di
        call    dir_commit
        call    rd_flush

        mov     word ptr [rd_opmode],2
        mov     si,[rd_opvol]
        call    sub_fromdir
        jc      sc2_nohandles
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
sc2_ok:
        mov     bx,[rd_ophnd]
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        clc
        ret
sc2_exists:
        mov     ax,ERR_EXISTS
        jmp     short sc2_out
sc2_full:
        mov     ax,ERR_NOHANDLES
        jmp     short sc2_out
sc2_nohandles:
        mov     ax,ERR_NOHANDLES
        jmp     short sc2_out
sc2_bad:
        mov     ax,ERR_NOPATH
sc2_out:
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        stc
        ret

        ; A REDIRECTOR'S DRIVE: DOS_Create's 1117h, pushed the attribute
        ; in AL - a volume label alone, or the file bits, and archive
        ; either way - and in AH whether an existing file is refused (5Bh)
        ; rather than truncated.  The entry's mode is read and write,
        ; compatibility, as DOS gives every create; the search attribute
        ; is $Creat's, and no wildcards.
sc2_net:
        call    net_nometa
        jc      sc2_nofile
        mov     al,ATTR_HIDDEN OR ATTR_SYSTEM
        call    net_sattr
        mov     al,[rd_crattr]
        test    al,ATTR_VOLUME
        jz      sc2_nvol
        mov     al,ATTR_VOLUME
sc2_nvol:
        and     al,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_VOLUME
        or      al,ATTR_ARCHIVE
        mov     ah,[rd_crnew]
        mov     word ptr [net_parm],ax
        mov     word ptr [rd_opmode],2
        mov     ax,1117h
        call    net_open                ; -> BX = the handle
        jc      sc2_out
        jmp     sc2_ok
sc2_nofile:
        mov     ax,ERR_NOFILE
        jmp     short sc2_out

;--- rd_stamp - now, in directory-entry form -> AX = time, DX = date.
;--- Every other register is preserved.
;
; UNTIL v0.60 IT PRESERVED ONLY CX, and both of the others it lost
; mattered.  The century was multiplied with a sixteen-bit MUL, which
; writes DX - where INT 1Ah had just put the month and the day - so
; every date came out as month 0, day 0.  And BX came back holding the
; year, while its one caller, sub_create, had the new directory entry's
; offset in BX and went on to write the time and date through it: into
; rd_dirbuf + 26 + 16h, and ten zeroes into rd_dirbuf + 26 + 0Ch.  That
; is the middle of the SECOND entry in the sector.  A file RMDOS
; created cut that entry's name to six characters and wiped its
; attribute - AUTOEXEC.BAT, in the run that found it.
rd_stamp:
        push    bx
        push    cx
        push    si
        mov     ah,02h
        int     1Ah                     ; CH:CL:DH = hours:minutes:seconds, BCD
        jc      rst_none
        mov     al,ch
        call    bcd2bin
        movzx   si,al
        shl     si,11
        mov     al,cl
        call    bcd2bin
        movzx   ax,al
        shl     ax,5
        or      si,ax
        mov     al,dh
        call    bcd2bin
        shr     al,1
        movzx   ax,al
        or      si,ax                   ; SI = the time
        mov     ah,04h
        int     1Ah                     ; CH:CL = century:year, DH:DL = month:day
        jc      rst_nodate
        mov     al,ch
        call    bcd2bin
        mov     bl,100
        mul     bl                      ; EIGHT bits: AX = AL * BL, DX untouched
        mov     bx,ax
        mov     al,cl
        call    bcd2bin
        xor     ah,ah
        add     ax,bx
        sub     ax,1980
        shl     ax,9
        mov     bx,ax
        mov     al,dh
        call    bcd2bin
        xor     ah,ah
        shl     ax,5
        or      bx,ax
        mov     al,dl
        call    bcd2bin
        xor     ah,ah
        or      ax,bx
        mov     dx,ax                   ; DX = the date
        mov     ax,si
        pop     si
        pop     cx
        pop     bx
        ret
rst_nodate:
        mov     ax,si
        mov     dx,(1 SHL 5) OR 1       ; 1-Jan-1980
        pop     si
        pop     cx
        pop     bx
        ret
rst_none:
        xor     ax,ax
        mov     dx,(1 SHL 5) OR 1
        pop     si
        pop     cx
        pop     bx
        ret

;--- sub_read / sub_write / sub_seek / sub_close ---------------------
; BX = handle, ECX = bytes, ES:DX = buffer.  For EXEC's use.
sub_read:
        push    si
        push    di
        mov     di,dx                   ; BEFORE sft_get, not after: the
        call    sft_get                 ; argument is taken out of the
        jc      sr_bad                  ; register while it is still ours
        call    sft_isnet
        je      sr_net                  ; a program on a redirector's drive
IFDEF RMDOS_EDBG
        push    eax
        mov     eax,dword ptr [si+SFT_SIZE]
        mov     dword ptr [ed_sz],eax
        mov     eax,dword ptr [si+SFT_POS]
        mov     dword ptr [ed_pos],eax
        pop     eax
ENDIF
        mov     byte ptr [rd_iowr],0
        call    file_io
        pop     di
        pop     si
        ret                             ; file_io's carry, NOT a clc: this
                                        ; said "success" over every error it
                                        ; was ever handed, and a loader that
                                        ; is told a broken read worked will
                                        ; run whatever is in the buffer
sr_bad:
        pop     di
        pop     si
        stc
        ret
sr_net:
        call    net_readall             ; -> ECX, CF; ES:DI the buffer
        pop     di
        pop     si
        ret

sub_seek:
        push    si
        call    sft_get
        jc      ss_bad
        mov     dword ptr [si+SFT_POS],eax
        pop     si
        clc
        ret
ss_bad:
        pop     si
        stc
        ret

sub_close:
        push    es
        push    cx
        push    di
        call    sft_release
        mov     es,[rd_psp]
        call    jft_ptr
        cmp     bx,cx
        jae     sc3_done
        mov     byte ptr es:[bx+di],0FFh
sc3_done:
        pop     di
        pop     cx
        pop     es
        ret

;=====================================================================
; The devices
;=====================================================================
; dev_read and dev_write: SI = the SFT, ES:DI = the buffer, ECX = the
; count -> ECX = what moved.  CF and AX = an error only from a driver
; CONFIG.SYS loaded (RMDEV.INC); the devices of this DOS's own do not
; fail.
dev_read:
        cmp     byte ptr [si+SFT_DRIVE],DEV_DRV
        jne     dr_own
        jmp     drv_read
dr_own:
        push    di
        push    es
        mov     al,[si+SFT_DRIVE]
        cmp     al,DEV_CON
        jne     dr_none
        or      ecx,ecx
        jz      dr_done
        mov     dx,cx
        xor     cx,cx
dr_loop:
        push    dx
        push    cx
        push    di
        push    es
        call    con_in
        pop     es
        pop     di
        pop     cx
        pop     dx
        cmp     al,13
        je      dr_eol
        stosb
        inc     cx
        cmp     cx,dx
        jb      dr_loop
        jmp     short dr_fin
dr_eol:
        mov     al,13
        stosb
        inc     cx
        cmp     cx,dx
        jae     dr_fin
        mov     al,10
        stosb
        inc     cx
dr_fin:
        movzx   ecx,cx
dr_done:
        pop     es
        pop     di
        clc
        ret
dr_none:
        xor     ecx,ecx
        pop     es
        pop     di
        clc
        ret

dev_write:
        cmp     byte ptr [si+SFT_DRIVE],DEV_DRV
        jne     dw_own
        jmp     drv_write
dw_own:
        push    si
        push    ds
        mov     al,[si+SFT_DRIVE]
        cmp     al,DEV_NUL
        je      dw_sink
        cmp     al,DEV_CON
        jne     dw_sink
        or      ecx,ecx
        jz      dw_done
        push    ecx
        mov     si,di
        push    es
        pop     ds
dw_loop:
        lodsb
        mov     dl,al
        push    ecx
        push    si
        push    ds
        push    cs
        pop     ds
        call    con_out
        pop     ds
        pop     si
        pop     ecx
        dec     ecx
        jnz     dw_loop
        pop     ecx
dw_done:
        pop     ds
        pop     si
        clc
        ret
dw_sink:
        pop     ds
        pop     si
        clc
        ret

;=====================================================================
; Data
;=====================================================================
rd_iowr     db  0
rd_iolba    dd  0
rd_iooff    dd  0
rd_iolen    dd  0
rd_iodone   dd  0

rd_opmode   dw  0
rd_opvol    dw  0
rd_ophnd    dw  0
rd_opidx    db  0
rd_crattr   db  0
rd_crnew    db  0
rd_crdir    dd  0
rd_tmp      dw  0

rd_ffvol    dw  0
rd_ffdrv    db  0                       ; the drive the search is on
rd_ffdir    dd  0
rd_ffname   db  11 dup(0)
rd_ffattr   db  0
rd_ffsec    dd  0
rd_ffclus   dd  0
rd_ffleft   dd  0
rd_ffroot   db  0
rd_ffent    dw  0
