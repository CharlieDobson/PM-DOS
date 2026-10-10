;=====================================================================
; RMFILE.INC - the FAT, the directories, the handles, and the calls
;              that reach them
;
; FAT12, FAT16 and FAT32, read and write.  8.3 names only: Windows 3.x
; has no long names to ask for, and an LFN implementation here would
; be a second one to keep in step with the kernel's for no caller.
;
; THREE CACHES AND NO MORE.  One FAT window, one directory sector, one
; data sector.  DOS had BUFFERS= and a chain; this has three fixed
; buffers, because an excursion is short, the working set is one file
; at a time, and a cache with a replacement policy is a cache with a
; coherency bug waiting in it.  Everything dirty is written before
; anything else is read into the same buffer, which is the whole of
; the policy.
;=====================================================================

;--- the system file table -------------------------------------------
; MS-DOS 4'S ENTRY, 3Bh BYTES, WITH DOS'S FIELDS WHERE DOS KEEPS THEM.
; Until v0.63 an entry was 30h bytes in an order of its own, which
; nothing minded until Windows in 386 enhanced mode: DOSMGR opens CON
; five times and then walks this table, taking an entry to be 3Bh
; bytes because the DOS says it is version 6, to find the five it
; planted - and closed handle FFFFh five times when it could not.  The
; fields a reader knows are DOS's now; RMDOS's own go in the bytes DOS
; gives to SHARE, which has nothing here to keep in them.
SFT_REF         EQU 00h                 ; dw  handles pointing here, 0 = free
SFT_MODE        EQU 02h                 ; dw  as 3Dh was given it
SFT_ATTR        EQU 04h                 ; db
SFT_DEVINFO     EQU 05h                 ; dw  DOS's device information word -
                                        ;     bit 7 a character device, and
                                        ;     a file's drive in bits 0-5;
                                        ;     what 4400h answers
SFT_DEVPTR      EQU 07h                 ; dd  the device header, far, or
                                        ;     FFFF:FFFF for a file (DOS puts
                                        ;     the DPB there; there is none)
SFT_CLUSLO      EQU 0Bh                 ; dw  the first cluster, low word, as
                                        ;     DOS has it - SFT_START is ours
SFT_TIME        EQU 0Dh                 ; dw
SFT_DATE        EQU 0Fh                 ; dw
SFT_SIZE        EQU 11h                 ; dd
SFT_POS         EQU 15h                 ; dd
SFT_DIROFF      EQU 19h                 ; dw  where in its sector the entry is
                                        ;     (DOS: the relative cluster)
SFT_DIRLBA      EQU 1Bh                 ; dd  the sector the entry is in, as
                                        ;     DOS keeps it
SFT_NAME        EQU 20h                 ; 11 bytes, the 8.3 form
SFT_FLAGS       EQU 2Bh                 ; db  SFF_* - SHARE's bytes from here
SFT_DRIVE       EQU 2Ch                 ; db  0 = A, or a device index
SFT_START       EQU 2Dh                 ; dd  first cluster
SFT_CURCLUS     EQU 31h                 ; dd  the cluster POS is in
SFT_CURIDX      EQU 35h                 ; dd  and which index that is
SFT_LEN         EQU 3Bh

SFF_DEVICE      EQU 01h                 ; a character device, not a file
SFF_WRITTEN     EQU 02h                 ; the directory entry needs updating
SFF_EOF         EQU 04h
SFF_STAMPED     EQU 08h                 ; 5701h set the time and date: a
                                        ; write after it does not re-stamp

;--- A REDIRECTOR'S FILE IS DOS'S SHAPE THROUGHOUT.  The redirector fills
;--- the entry in (RMNET.INC) and keeps what it likes in the bytes this
;--- DOS keeps SFF_* and the rest in, so for one of its files only DOS's
;--- own fields mean anything here - and the first thing every handle
;--- call asks is whether the entry is one, which is sft_isnet's
;--- question (RMFILE2.INC) and not NF_ISNET's alone: this DOS's own
;--- devices have the same bit set.  Bits of the HIGH byte of DOS's
;--- flags word (SFT_DEVINFO) unless they say otherwise; the names are
;--- not NET_*, which wasm would fold onto RMNET's labels.
NF_ISNET        EQU 80h                 ; sf_isnet: the redirector's file
NF_NODATE       EQU 40h                 ; sf_close_nodate: 5701h dated it
NF_NOINH        EQU 10h                 ; sf_no_inherit
NF_CLEAN        EQU 40h                 ; devid_file_clean, the LOW byte
NC_ISNET        EQU 80h                 ; curdir_isnet, the high byte of
                                        ; CDS_FLAGS: the redirector's drive

SFT_COUNT       EQU 80                  ; Windows keeps seventeen files open
                                        ; at once before the shell has even
                                        ; started; forty was the number a
                                        ; test program needed
JFT_COUNT       EQU 20                  ; what fits in a PSP, and what a
                                        ; process starts with.  It is NOT the
                                        ; limit: 67h moves the table into a
                                        ; block of its own and makes it
                                        ; bigger, and Windows moves it without
                                        ; asking at all - see jft_ptr in
                                        ; RMFILE2.INC, and NOTES.TXT v0.55.

;--- devices, in the order the standard handles open them ------------
DEV_CON         EQU 0
DEV_AUX         EQU 1
DEV_PRN         EQU 2
DEV_NUL         EQU 3
DEV_DRV         EQU 4                   ; a driver CONFIG.SYS loaded, whose
                                        ; header SFT_DEVPTR names - RMDEV.INC

;--- a directory entry, which has not moved since 1983 ---------------
DE_NAME         EQU 00h
DE_ATTR         EQU 0Bh
DE_TIME         EQU 16h
DE_DATE         EQU 18h
DE_CLUSHI       EQU 14h
DE_CLUSLO       EQU 1Ah
DE_SIZE         EQU 1Ch
DE_LEN          EQU 32

;--- the current directory of each drive -----------------------------
; In the block below 64K (RMDOS.INC, RL_CDS), LASTDRIVE of them, and
; each the size and shape of MS-DOS 4's, because DOSMGR instances them
; per virtual machine and asks the size through the 1607h callout.
; The fields RMDOS reads sit where DOS keeps the same things; the rest
; of each entry is zero and nothing reads it.
;
; THE PATH IS DOS'S OWN, "C:\DIR\SUB", drive and root and all, since
; 2026-10-06.  It was the part after "C:\" alone, which is what 47h
; answers and so what this DOS mostly wants - but Windows for
; Workgroups' IFSMGR.386 reads and writes these entries as DOS keeps
; them: claiming a drive, it writes the zero at +3 that cuts the path
; back to "C:\", which here was the fourth character of a directory.
; So the text starts at 0 and CDS_PATH names the part after the root,
; at 3, and every read and write of the path in this DOS goes on being
; one of that part, unchanged.
CDS_TEXT        EQU 00h                 ; 67 bytes, ASCIIZ: "C:\DIR\SUB"
CDS_PATH        EQU 03h                 ; ...and from the first directory on:
                                        ; 64 bytes, what 47h wants
CDS_FLAGS       EQU 43h                 ; dw  4000h = a drive that exists
CDS_DPB         EQU 45h                 ; dd  the drive's parameter block, at
                                        ; RL_DPB; FFFF:FFFF with no drive
CDS_CLUS        EQU 49h                 ; dd  0 = the root (DOS's own word is
                                        ; here; the dword after it is one
                                        ; nothing reads)
CDS_BACK        EQU 4Fh                 ; dw  2: the root backslash's place
CDS_LEN         EQU RL_CDSLEN           ; 58h

;=====================================================================
; THE FAT WINDOW
;
; Two sectors, not one, and for one reason: a FAT12 entry may straddle
; a sector boundary, and with the following sector already in the
; buffer the word read at offset 511 simply spills into it and needs
; no special case anywhere.  The same trick is in BOOT\FATLOAD.INC and
; for the same reason.
;=====================================================================

;--- fat_win - SI = volume, EAX = the FAT sector wanted.  Loads the
;--- pair starting there if it is not already in.
fat_win:
        pushad
        push    es
        cmp     si,[rd_fatvol]
        jne     fw_load
        cmp     eax,dword ptr [rd_fatlba]
        je      fw_done
fw_load:
        call    fat_flush
        mov     dword ptr [rd_fatlba],eax
        mov     [rd_fatvol],si
        add     eax,dword ptr [si+VOL_FATLBA]
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_fatbuf
        mov     cx,2
        call    rd_rdsec
        jnc     fw_done
        mov     dword ptr [rd_fatlba],-1        ; do not believe what is in
        mov     word ptr [rd_fatvol],0          ; the buffer next time
fw_done:
        pop     es
        popad
        ret

;--- fat_flush - write the window back if anything changed, to EVERY
;--- copy of the FAT.  A second FAT that does not match the first is
;--- what CHKDSK exists to complain about.
fat_flush:
        pushad
        push    es
        cmp     byte ptr [rd_fatdirty],0
        je      ff_done
        mov     si,[rd_fatvol]
        or      si,si
        jz      ff_clean
        mov     bx,cs
        mov     es,bx
        movzx   cx,byte ptr [si+VOL_NFATS]
        or      cx,cx
        jz      ff_clean
        mov     dword ptr [rd_fatn],0
ff_next:
        mov     eax,dword ptr [rd_fatn]
        mul     dword ptr [si+VOL_SPF]
        add     eax,dword ptr [rd_fatlba]
        add     eax,dword ptr [si+VOL_FATLBA]
        push    cx
        mov     bx,OFFSET rd_fatbuf
        mov     cx,2
        call    rd_wrsec
        pop     cx
        inc     dword ptr [rd_fatn]
        loop    ff_next
ff_clean:
        mov     byte ptr [rd_fatdirty],0
ff_done:
        pop     es
        popad
        ret

;--- fat_get - SI = volume, EAX = cluster -> EAX = its entry ---------
fat_get:
        push    ebx
        push    ecx
        push    edx
        push    di
        mov     ebx,eax
        movzx   ecx,byte ptr [si+VOL_FATTYPE]
        cmp     cl,12
        je      fg_12
        cmp     cl,16
        je      fg_16

;--- FAT32: four bytes each, and the top four bits are not ours ------
        shl     eax,2
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     eax,dword ptr [rd_fatbuf + di]
        and     eax,0FFFFFFFh
        jmp     short fg_done

fg_16:
        shl     eax,1
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        movzx   eax,word ptr [rd_fatbuf + di]
        ; WIDENED LIKE FAT12's, and until v0.60 it was not.  fat_eoc
        ; tests one number for every FAT type, 0FFFFFF8h, so a FAT16
        ; end of chain came back as FFFFh - "not the end" - and every
        ; walker took it for a cluster.  Reads never noticed: they stop
        ; at the file size.  Everything that FOLLOWS a chain to its end
        ; did: extending a file hooked the new cluster onto whatever
        ; FAT entry FFFFh was and wrote the next sector to 131426 of a
        ; 65520-sector disk (WINOLDAP's swap file, the first write
        ; Windows ever made through RMDOS that needed a second
        ; cluster), and freeing a chain zeroed "entry FFFFh", which is
        ; past the FAT.  TOOLS\RMWRTEST.ASM is the test.
        cmp     eax,0FFF8h
        jb      fg_done
        or      eax,0FFF0000h
        jmp     short fg_done

;--- FAT12: twelve bits, so the byte offset is cluster + cluster/2 ---
fg_12:
        mov     edx,eax
        shr     edx,1
        add     eax,edx
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        movzx   eax,word ptr [rd_fatbuf + di]
        test    bl,1
        jz      fg_12even
        shr     eax,4
        jmp     short fg_12done
fg_12even:
        and     eax,0FFFh
fg_12done:
        ; A FAT12 end-of-chain is FF8h and up; widen it so every caller
        ; can test against one number whatever the volume is.
        cmp     eax,0FF8h
        jb      fg_done
        or      eax,0FFFF000h
fg_done:
        pop     di
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- fat_put - SI = volume, EAX = cluster, EDX = the value -----------
fat_put:
        pushad
        mov     ebx,eax
        mov     ebp,edx                 ; BP is not a frame pointer in here
        movzx   ecx,byte ptr [si+VOL_FATTYPE]
        cmp     cl,12
        je      fp_12
        cmp     cl,16
        je      fp_16

        shl     eax,2
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     eax,dword ptr [rd_fatbuf + di]
        and     eax,0F0000000h          ; the reserved top nibble stays
        and     ebp,0FFFFFFFh
        or      eax,ebp
        mov     dword ptr [rd_fatbuf + di],eax
        jmp     short fp_done

fp_16:
        shl     eax,1
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     word ptr [rd_fatbuf + di],bp
        jmp     short fp_done

fp_12:
        mov     edx,eax
        shr     edx,1
        add     eax,edx
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     ax,word ptr [rd_fatbuf + di]
        test    bl,1
        jz      fp_12even
        and     ax,000Fh                ; keep the low nibble of the pair
        mov     cx,bp
        shl     cx,4
        or      ax,cx
        jmp     short fp_12store
fp_12even:
        and     ax,0F000h               ; keep the high nibble
        mov     cx,bp
        and     cx,0FFFh
        or      ax,cx
fp_12store:
        mov     word ptr [rd_fatbuf + di],ax
fp_done:
        mov     byte ptr [rd_fatdirty],1
        popad
        ret

;--- fat_eoc - EAX = a FAT entry.  CF set if it ends a chain ---------
fat_eoc:
        cmp     eax,0FFFFFF8h
        jb      fe_no
        stc
        ret
fe_no:
        clc
        ret

;--- fat_alloc - SI = volume -> EAX = a cluster, marked as the end of
;--- a chain, or zero if the volume is full.
;
; The search starts where the last one finished rather than at cluster
; two, which turns filling a disk from something quadratic into
; something linear.  It wraps once, so a volume with a hole near the
; start is still filled.
fat_alloc:
        push    ebx
        push    ecx
        push    edx
        mov     ebx,dword ptr [si+VOL_FREEHINT]
        cmp     ebx,2
        jae     fa_hinted
        mov     ebx,2
fa_hinted:
        mov     ecx,dword ptr [si+VOL_CLUSTERS]
        add     ecx,2                   ; the highest legal number, plus one
        mov     edx,ecx
        sub     edx,2
        inc     edx                     ; how many to look at before giving up
fa_scan:
        or      edx,edx
        jz      fa_full
        dec     edx
        cmp     ebx,ecx
        jb      fa_inrange
        mov     ebx,2
fa_inrange:
        mov     eax,ebx
        call    fat_get
        or      eax,eax
        jz      fa_got
        inc     ebx
        jmp     short fa_scan
fa_got:
        mov     eax,ebx
        push    eax
        mov     edx,0FFFFFFFh
        call    fat_put
        pop     eax
        mov     ebx,eax
        inc     ebx
        mov     dword ptr [si+VOL_FREEHINT],ebx
        pop     edx
        pop     ecx
        pop     ebx
        ret
fa_full:
        xor     eax,eax
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- fat_freechain - SI = volume, EAX = the first cluster ------------
;
; The end test is made on the NEXT value, the one just read.  Until
; v0.60 it was made on EAX after fat_put - which preserves EAX, so it
; was the cluster just freed, a small number that is never an end - and
; the loop went on to free "cluster 0FFFFFFFh", reading and writing the
; FAT sector that number lands in: sector 1048647 of a 32MB disk.
fat_freechain:
        pushad
        mov     ebx,eax
fc_loop:
        cmp     ebx,2
        jb      fc_done
        mov     eax,ebx
        call    fat_get                 ; EAX = what comes next
        push    eax
        mov     eax,ebx
        xor     edx,edx
        call    fat_put                 ; this one is free
        pop     eax
        call    fat_eoc
        jc      fc_done                 ; that was the last of it
        mov     ebx,eax
        jmp     short fc_loop
fc_done:
        popad
        ret

;--- rd_countfree - SI = volume -> EAX = free clusters ---------------
rd_countfree:
        push    ebx
        push    ecx
        push    edx
        xor     edx,edx                 ; the running count
        mov     ebx,2
        mov     ecx,dword ptr [si+VOL_CLUSTERS]
        add     ecx,2
cf_loop:
        cmp     ebx,ecx
        jae     cf_done
        mov     eax,ebx
        call    fat_get
        or      eax,eax
        jnz     cf_used
        inc     edx
cf_used:
        inc     ebx
        jmp     short cf_loop
cf_done:
        mov     eax,edx
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- clus2lba - SI = volume, EAX = cluster -> EAX = its first sector -
clus2lba:
        push    ecx
        sub     eax,2
        movzx   ecx,byte ptr [si+VOL_SPC]
        mul     ecx
        add     eax,dword ptr [si+VOL_DATALBA]
        pop     ecx
        ret

;=====================================================================
; THE DIRECTORY SECTOR CACHE
;=====================================================================

;--- dir_win - SI = volume, EAX = an absolute LBA -> that sector is in
;--- rd_dirbuf.  CF on a read error.
dir_win:
        pushad
        push    es
        cmp     si,[rd_dirvol]
        jne     dw_load
        cmp     eax,dword ptr [rd_dirlba]
        je      dw_ok
dw_load:
        call    dir_flush
        mov     dword ptr [rd_dirlba],eax
        mov     [rd_dirvol],si
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_dirbuf
        mov     cx,1
        call    rd_rdsec
        jnc     dw_ok
        mov     dword ptr [rd_dirlba],-1
        mov     word ptr [rd_dirvol],0
        pop     es
        popad
        stc
        ret
dw_ok:
        pop     es
        popad
        clc
        ret

dir_flush:
        pushad
        push    es
        cmp     byte ptr [rd_dirdirty],0
        je      df_done
        mov     si,[rd_dirvol]
        or      si,si
        jz      df_clean
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_dirbuf
        mov     eax,dword ptr [rd_dirlba]
        mov     cx,1
        call    rd_wrsec
df_clean:
        mov     byte ptr [rd_dirdirty],0
df_done:
        pop     es
        popad
        ret

;--- rd_flush - everything, everywhere.  0Dh and every close call it.
rd_flush:
        call    fat_flush
        call    dir_flush
        ret

;=====================================================================
; NAMES
;
; rd_name83 - DS:SI -> one path component, ES:DI -> eleven bytes.
;             Turns "readme.txt" into "README  TXT".  Stops at a
;             separator or a NUL and leaves SI on it.
;
; A NAME THAT IS TOO LONG IS TRUNCATED, NOT REFUSED, which is what DOS
; did: "verylongname.text" opens "VERYLONG.TEX".  Refusing would be
; more honest and would also mean a program that has always worked
; stops working, so it truncates.
;
; AN ASTERISK FILLS THE REST OF ITS FIELD WITH QUESTION MARKS, which is
; the whole of what '*' means to DOS: "*.*" is "???????????" and
; "A*.T*" is "A???????T??".  Every routine below compares eleven bytes,
; and '?' is the only wildcard any of them knows - so an asterisk that
; is not expanded HERE is a literal asterisk down there, and matches a
; file with an asterisk in its name, of which there are none.
;
; That was v0.57's "File Manager lists an empty directory".  4Eh was
; not broken and neither was the directory walk: the search template
; every caller passes could not match anything, so every search in
; RMDOS returned "no more files" on its first call.  It was invisible
; for as long as it was, because rd_name83's OTHER caller is rd_path
; resolving a path to open, and a path to open has no wildcards in it.
;=====================================================================
rd_name83:
        push    ax
        push    cx
        push    di
        mov     al,' '
        mov     cx,11
        cld
        rep     stosb
        sub     di,11

        mov     cx,8
n83_base:
        mov     al,[si]
        cmp     al,0
        je      n83_done
        cmp     al,'\'
        je      n83_done
        cmp     al,'/'
        je      n83_done
        cmp     al,'.'
        je      n83_dot
        cmp     al,'*'
        je      n83_bstar
        inc     si
        or      cx,cx
        jz      n83_base                ; past eight: swallow it
        call    rd_upcase
        mov     [di],al
        inc     di
        dec     cx
        jmp     short n83_base
n83_bstar:
        ; The rest of the eight, as question marks.  CX comes back zero,
        ; so the loop swallows whatever else was written before the dot
        ; - "ab*cd.txt" is "AB??????TXT" on DOS too.
        inc     si
        mov     al,'?'
        rep     stosb
        jmp     short n83_base
n83_dot:
        inc     si
        pop     di
        push    di
        add     di,8
        mov     cx,3
n83_ext:
        mov     al,[si]
        cmp     al,0
        je      n83_done
        cmp     al,'\'
        je      n83_done
        cmp     al,'/'
        je      n83_done
        cmp     al,'*'
        je      n83_estar
        inc     si
        or      cx,cx
        jz      n83_ext
        call    rd_upcase
        mov     [di],al
        inc     di
        dec     cx
        jmp     short n83_ext
n83_estar:
        inc     si
        mov     al,'?'
        rep     stosb
        jmp     short n83_ext
n83_done:
        pop     di
        pop     cx
        pop     ax
        ret

;--- rd_upcase - AL, in place.  ASCII only, which is the whole of what
;--- codepage 437 upper-cases and what PM-DOS does too.
rd_upcase:
        cmp     al,'a'
        jb      uc_done
        cmp     al,'z'
        ja      uc_done
        sub     al,20h
uc_done:
        ret

;=====================================================================
; rd_canon - ES:SI -> a path in the CALLER's segment.  Builds the one
;            true spelling of it in rd_canbuf:
;
;               "D:\DIR\SUB\NAME.EXT"
;
;            upper case; every component in the 8.3 form rd_name83
;            makes, so "verylongname.text" is "VERYLONG.TEX" and "*" is
;            "????????"; "." dropped, and ".." taking the component
;            before it with it; a relative path put after the drive's
;            current directory.  NOTHING IS LOOKED UP - this is text,
;            as DOS's own TRUENAME is, and 60h answers with it as it
;            stands.
;
;            -> AL = drive, CF clear.  CF: AX = ERR_BADDRIVE for a
;            letter past LASTDRIVE, ERR_NOPATH for an empty string, a
;            ".." above the root, or a result too long to be a path.
;            DS must be ours.  Everything but AX is preserved.
;=====================================================================
RC_MAX          EQU     128             ; MS-DOS's OPENBUF: a redirector's root
                                        ; can be long, and "D:\" + 64 +
                                        ; "\NAME.EXT" fits with room over
RC_ROOTMAX      EQU     66              ; the longest root a structure's text
                                        ; holds before its NUL
RC_LOCAL        EQU     80              ; ...and the most a name on a drive
                                        ; of this DOS's own may be, which
                                        ; was RC_MAX until a redirector's
                                        ; roots needed more: a deeper local
                                        ; path is one MS-DOS refuses too
rd_canon:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es

        ; The string into our own segment first - see rd_path's comment
        ; history in NOTES.TXT: every routine under this one reads its
        ; data through DS, and DS stays ours.
        push    ds
        push    es
        pop     ds                      ; DS:SI is the caller's string
        push    cs
        pop     es
        mov     di,OFFSET rd_pathbuf
        mov     cx,RD_PATHMAX - 1
        cld
rc_copy:
        lodsb
        stosb
        or      al,al
        jz      rc_copied
        loop    rc_copy
        mov     byte ptr es:[di],0      ; the loop ran out before the string
rc_copied:
        pop     ds
        mov     si,OFFSET rd_pathbuf
        cmp     byte ptr [si],0
        je      rc_nopath               ; "" names nothing, as in DOS

        mov     al,[rd_drive]
        cmp     byte ptr [si+1],':'
        jne     rc_nodrive
        mov     al,[si]
        call    rd_upcase
        sub     al,'A'
        add     si,2
rc_nodrive:
        cmp     al,[rd_ncds]
        jae     rc_baddrive
        mov     [rc_drv],al

        ; THE ROOT IS THE DRIVE'S OWN, as TransPath takes it: the text of
        ; its current directory structure up to curdir_end.  For every
        ; drive of this DOS's that is "D:"; a redirector's can be longer
        ; - "\\SERVER\SHARE" - and the name it is handed starts with it,
        ; as MS-DOS hands it one.  Only a redirector's structure is
        ; believed past two, and only up to what the text can hold.
        push    si
        push    fs
        mov     fs,word ptr cs:[rd_lowseg]
        movzx   si,al
        imul    si,si,CDS_LEN
        add     si,RL_CDS               ; FS:SI = the structure
        mov     di,OFFSET rd_canbuf
        mov     cx,2
        test    byte ptr fs:[si+CDS_FLAGS+1],NC_ISNET
        jz      rc_letter
        mov     cx,word ptr fs:[si+CDS_BACK]
        cmp     cx,2
        jbe     rc_letter2
        cmp     cx,RC_ROOTMAX
        ja      rc_letter2
        mov     [rc_back],cx
        mov     word ptr [rc_lim],RC_MAX - 14
rc_rootcpy:
        mov     al,fs:[si]
        inc     si
        stosb
        loop    rc_rootcpy
        jmp     short rc_rooted
rc_letter2:
        mov     cx,2
rc_letter:
        mov     [rc_back],cx
        mov     word ptr [rc_lim],RC_LOCAL - 14 ; a drive of ours: as long
                                        ; as MS-DOS lets a local name be
        mov     al,[rc_drv]
        add     al,'A'
        stosb
        mov     al,':'
        stosb
rc_rooted:
        mov     al,'\'
        stosb
        mov     [rc_root],di            ; just past the root's backslash:
        pop     fs                      ; ".." goes no further up than this
        pop     si

        mov     al,[si]
        cmp     al,'\'
        je      rc_abs
        cmp     al,'/'
        je      rc_abs

        ; Relative: the drive's current directory goes first - out of
        ; the block below 64K, through FS, from the end of the root on
        push    si
        push    fs
        mov     fs,word ptr cs:[rd_lowseg]
        movzx   ax,byte ptr [rc_drv]
        mov     cx,CDS_LEN
        mul     cx
        mov     si,ax
        add     si,RL_CDS
        add     si,[rc_back]
        cmp     byte ptr fs:[si],'\'
        jne     rc_cdsfrom
        inc     si
rc_cdsfrom:
        cmp     byte ptr fs:[si],0
        je      rc_cdsdone
rc_cdscopy:
        mov     al,fs:[si]
        inc     si
        or      al,al
        jz      rc_cdsend
        stosb
        jmp     short rc_cdscopy
rc_cdsend:
        mov     al,'\'
        stosb
rc_cdsdone:
        pop     fs
        pop     si
        jmp     short rc_comp
rc_abs:
        inc     si

; DI is always just past a '\' here.
rc_comp:
        mov     al,[si]
        or      al,al
        jz      rc_end
        cmp     al,'\'
        je      rc_skip                 ; "\\" is one separator
        cmp     al,'/'
        je      rc_skip
        cmp     al,'.'
        jne     rc_name
        mov     ah,[si+1]
        call    rc_issep
        jne     rc_notdot
        inc     si                      ; "." - where we are
        jmp     short rc_comp
rc_notdot:
        cmp     ah,'.'
        jne     rc_name
        mov     ah,[si+2]
        call    rc_issep
        jne     rc_name
        add     si,2                    ; ".." - up one
        cmp     di,[rc_root]
        je      rc_nopath               ; there is nothing above the root
        dec     di                      ; onto the '\' that ends the last one
rc_up:
        dec     di
        cmp     byte ptr [di],'\'
        jne     rc_up
        inc     di
        jmp     short rc_comp
rc_skip:
        inc     si
        jmp     short rc_comp

rc_name:
        mov     ax,di
        sub     ax,OFFSET rd_canbuf
        cmp     ax,[rc_lim]             ; room for one more 8.3 name and a '\'
        ja      rc_nopath
        push    di
        mov     di,OFFSET rc_n83
        call    rd_name83               ; SI is left on the separator
        pop     di
        push    si
        mov     si,OFFSET rc_n83        ; the base, without its padding
        mov     cx,8
rc_base:
        lodsb
        cmp     al,' '
        je      rc_baseend
        stosb
        loop    rc_base
rc_baseend:
        mov     si,OFFSET rc_n83 + 8
        cmp     byte ptr [si],' '
        je      rc_noext
        mov     al,'.'
        stosb
        mov     cx,3
rc_ext:
        lodsb
        cmp     al,' '
        je      rc_noext
        stosb
        loop    rc_ext
rc_noext:
        pop     si
        mov     al,'\'
        stosb
        jmp     rc_comp

rc_end:
        ; The '\' after the last component goes, unless it is the root's
        cmp     di,[rc_root]
        je      rc_term
        dec     di
rc_term:
        mov     byte ptr [di],0
        mov     al,[rc_drv]
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
rc_baddrive:
        mov     ax,ERR_BADDRIVE
        jmp     short rc_fail
rc_nopath:
        mov     ax,ERR_NOPATH
rc_fail:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

;--- rc_issep - ZF if AH is a NUL or a path separator
rc_issep:
        or      ah,ah
        jz      ris_out
        cmp     ah,'\'
        je      ris_out
        cmp     ah,'/'
ris_out:
        ret

;=====================================================================
; rd_path - resolve the caller's path.
;
;   ES:SI -> the path, in the CALLER's segment
;   Returns: AL = drive, EBX = the directory's start cluster (0 = the
;            root of a FAT12/16 volume), rd_fcb = the final component
;            as eleven bytes, SI = volume record.  CF on error.
;
; The last component is NOT looked up - that is the caller's business,
; because create and open want different things when it is missing.
;
; SINCE v0.60 THE WALK IS OF THE CANONICAL PATH, always from the root.
; rd_canon turns whatever the caller wrote into "D:\DIR\NAME.EXT" with
; "." and ".." already resolved, which is the one thing the walk below
; could never do: rd_name83 has no idea what ".." is, it made it a
; blank name with a "." extension, and "CD .." was "path not found" on
; RMDOS for fifteen releases.  Relative paths now start from the text
; in the CDS, which 3Bh keeps, rather than from a cluster number that
; had no path beside it.
;=====================================================================
rd_path:
        push    cx
        push    dx
        push    di
        push    bp
        push    es

        call    rd_canon                ; -> rd_canbuf, AL = drive
        jc      rp_bad
        push    cs
        pop     es
        mov     [rp_drv],al

        ; Mount it before walking it, so a bad drive letter fails here
        ; rather than three components in.
        call    rd_getvol
        jc      rp_bad
        mov     [rp_vol],si

        xor     ebx,ebx                 ; the root
        mov     bp,OFFSET rd_canbuf + 3 ; past "D:\"

rp_comp:
        ; One component into rd_fcb.  rd_name83 reads DS:SI and writes
        ; ES:DI, and both are ours.
        mov     si,bp
        mov     di,OFFSET rd_fcb
        call    rd_name83
        mov     bp,si
        cmp     byte ptr [bp],'\'
        jne     rp_last                 ; that was the last of them

        inc     bp
        mov     si,[rp_vol]
        mov     eax,ebx
        mov     di,OFFSET rd_fcb
        call    dir_find
        jc      rp_bad
        mov     si,[rd_deoff]
        test    byte ptr [rd_dirbuf + si + DE_ATTR],ATTR_DIR
        jz      rp_bad                  ; a file cannot be walked through
        call    rd_declus               ; -> EBX
        jmp     short rp_comp

rp_last:
        mov     al,[rp_drv]
        mov     si,[rp_vol]
        pop     es
        pop     bp
        pop     di
        pop     dx
        pop     cx
        clc
        ret
rp_bad:
        pop     es
        pop     bp
        pop     di
        pop     dx
        pop     cx
        mov     ax,ERR_NOPATH
        stc
        ret
;--- rd_declus - the cluster of the entry sitting in rd_dirbuf at
;--- rd_deoff, into EBX.  Both halves, because FAT32 uses both.
rd_declus:
        push    si
        mov     si,word ptr cs:[rd_deoff]
        movzx   ebx,word ptr cs:[rd_dirbuf + si + DE_CLUSHI]
        shl     ebx,16
        mov     bx,word ptr cs:[rd_dirbuf + si + DE_CLUSLO]
        pop     si
        ret

;--- rd_cdsclus - AL = drive -> EBX = where it is standing -----------
rd_cdsclus:
        push    ax
        push    si
        push    fs
        mov     fs,word ptr cs:[rd_lowseg]
        movzx   si,al
        mov     ax,CDS_LEN
        mul     si
        mov     si,ax
        add     si,RL_CDS
        mov     ebx,dword ptr fs:[si+CDS_CLUS]
        pop     fs
        pop     si
        pop     ax
        ret

;=====================================================================
; dir_find - SI = volume, EAX = the directory's start cluster (0 = the
;            root of a FAT12/16 volume), DI -> eleven bytes.
;
; Found: CF clear, the entry's sector is in rd_dirbuf and rd_deoff is
; its offset in it.  Not found: CF set.
;
; ONE ROUTINE WALKS BOTH KINDS OF DIRECTORY.  A FAT12/16 root is a
; fixed run of sectors and everything else is a cluster chain, and the
; only difference the search sees is how it gets the next sector - so
; that is the only thing the two paths do differently.
;=====================================================================
dir_find:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    di
        mov     word ptr [rd_dfname],di
        call    dir_first
        jc      dfi_no
dfi_sector:
        mov     eax,dword ptr [rd_dfsec]
        call    dir_win
        jc      dfi_no
        xor     bx,bx
dfi_entry:
        mov     al,byte ptr [rd_dirbuf + bx]
        or      al,al
        jz      dfi_no                  ; a zero name ends the directory
        cmp     al,0E5h
        je      dfi_next
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        and     al,0Fh
        cmp     al,0Fh
        je      dfi_next                ; a long-name fragment, skipped -
                                        ; the 8.3 entry it belongs to is
                                        ; along in a moment
        push    bx
        push    si
        push    di
        mov     di,[rd_dfname]
        lea     si,[rd_dirbuf + bx]
        mov     cx,11
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es
        cld
        repe    cmpsb
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     bx
        je      dfi_found
dfi_next:
        add     bx,DE_LEN
        cmp     bx,SECSIZE
        jb      dfi_entry
        call    dir_next
        jnc     dfi_sector
dfi_no:
        pop     di
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
dfi_found:
        mov     [rd_deoff],bx
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_delba],eax
        pop     di
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

;--- dir_first - SI = volume, EAX = start cluster (0 = root).  Sets up
;--- the walk state and leaves the first sector's LBA in rd_dfsec.
dir_first:
        mov     dword ptr [rd_dfclus],eax
        or      eax,eax
        jnz     dfr_sub

        ; The fixed root of a FAT12/16 volume.  A FAT32 volume has no
        ; such thing, and its root cluster stands in for it.
        cmp     byte ptr [si+VOL_FATTYPE],32
        jne     dfr_root
        mov     eax,dword ptr [si+VOL_ROOTCLUS]
        mov     dword ptr [rd_dfclus],eax
        jmp     short dfr_sub
dfr_root:
        mov     eax,dword ptr [si+VOL_ROOTLBA]
        mov     dword ptr [rd_dfsec],eax
        movzx   eax,word ptr [si+VOL_ROOTENT]
        shl     eax,5
        add     eax,SECSIZE - 1
        shr     eax,9
        mov     dword ptr [rd_dfleft],eax
        mov     byte ptr [rd_dfroot],1
        or      eax,eax
        jz      dfr_none
        clc
        ret
dfr_none:
        stc
        ret
dfr_sub:
        mov     byte ptr [rd_dfroot],0
        mov     eax,dword ptr [rd_dfclus]
        cmp     eax,2
        jb      dfr_none
        call    clus2lba
        mov     dword ptr [rd_dfsec],eax
        movzx   eax,byte ptr [si+VOL_SPC]
        mov     dword ptr [rd_dfleft],eax
        clc
        ret

;--- dir_next - the next sector of the directory being walked.  CF set
;--- when there are no more.
dir_next:
        push    eax
        dec     dword ptr [rd_dfleft]
        jz      dn_cluster
        inc     dword ptr [rd_dfsec]
        pop     eax
        clc
        ret
dn_cluster:
        cmp     byte ptr [rd_dfroot],0
        jne     dn_end                  ; a fixed root does not chain
        mov     eax,dword ptr [rd_dfclus]
        call    fat_get
        call    fat_eoc
        jc      dn_end
        cmp     eax,2
        jb      dn_end
        mov     dword ptr [rd_dfclus],eax
        call    clus2lba
        mov     dword ptr [rd_dfsec],eax
        movzx   eax,byte ptr [si+VOL_SPC]
        mov     dword ptr [rd_dfleft],eax
        pop     eax
        clc
        ret
dn_end:
        pop     eax
        stc
        ret

;=====================================================================
; dir_alloc - SI = volume, EAX = the directory's start cluster.  Find
;             a free entry, or grow the directory to make one.
;
; Leaves the sector in rd_dirbuf with rd_deoff pointing at 32 bytes
; that are the caller's to fill.  CF if the directory cannot grow.
;=====================================================================
dir_alloc:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     dword ptr [rd_dastart],eax
        call    dir_first
        jc      da_grow
da_sector:
        mov     eax,dword ptr [rd_dfsec]
        call    dir_win
        jc      da_fail
        xor     bx,bx
da_entry:
        mov     al,byte ptr [rd_dirbuf + bx]
        or      al,al
        je      da_take
        cmp     al,0E5h
        je      da_take
        add     bx,DE_LEN
        cmp     bx,SECSIZE
        jb      da_entry
        call    dir_next
        jnc     da_sector
da_grow:
        ; No room.  A subdirectory can be given another cluster; the
        ; fixed root of a FAT12/16 volume cannot, and that is the one
        ; hard limit DOS ever had on how many files a disk may hold.
        cmp     byte ptr [rd_dfroot],0
        jne     da_fail
        call    fat_alloc
        or      eax,eax
        jz      da_fail
        mov     ebx,eax                 ; the new cluster
        mov     eax,dword ptr [rd_dfclus]
        mov     edx,ebx
        call    fat_put                 ; chain it on
        mov     eax,ebx
        call    dir_zero
        jc      da_fail
        mov     eax,ebx
        call    clus2lba
        mov     dword ptr [rd_dfsec],eax
        call    dir_win
        jc      da_fail
        xor     bx,bx
da_take:
        mov     [rd_deoff],bx
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_delba],eax
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
da_fail:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;--- dir_zero - SI = volume, EAX = cluster.  Fill it with zeroes, which
;--- is what makes a fresh directory cluster read as "no more entries".
;
; THE LBA LIVES IN EDX, because BX is the buffer.  Until v0.60 it lived
; in EBX, and the "mov bx,OFFSET rd_datbuf" that sets up each write
; replaced its low half - so the first sector of the cluster was
; cleared and every later one was a sector of zeroes written to
; (LBA AND FFFF0000h) + rd_datbuf + 1: into the FAT, the root or
; somebody's file, every time a subdirectory grew on a volume with
; more than one sector to a cluster.
dir_zero:
        pushad
        push    es
        ; The directory window may hold a sector of this cluster from
        ; the last time it was somebody's directory: write out whatever
        ; is pending, and forget it once the disk has the zeroes.
        call    dir_flush
        call    clus2lba
        mov     edx,eax
        movzx   ecx,byte ptr [si+VOL_SPC]
        push    cx
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rd_datbuf
        mov     cx,SECSIZE
        xor     al,al
        cld
        rep     stosb
        pop     cx
dz_loop:
        mov     eax,edx
        push    cx
        mov     bx,OFFSET rd_datbuf
        mov     cx,1
        call    rd_wrsec
        pop     cx
        jc      dz_bad
        inc     edx
        loop    dz_loop
        mov     dword ptr [rd_dirlba],-1
        mov     word ptr [rd_dirvol],0
        pop     es
        popad
        clc
        ret
dz_bad:
        pop     es
        popad
        stc
        ret

;--- dir_commit - the entry at rd_deoff has been changed; mark the
;--- buffer so it goes out.
dir_commit:
        mov     byte ptr [rd_dirdirty],1
        ret

;=====================================================================
; Data
;=====================================================================
rd_fatbuf   db  (SECSIZE * 2) dup(0)
rd_fatlba   dd  -1
rd_fatvol   dw  0
rd_fatdirty db  0
rd_fatn     dd  0

rd_dirbuf   db  SECSIZE dup(0)
rd_dirlba   dd  -1
rd_dirvol   dw  0
rd_dirdirty db  0

rd_datbuf   db  SECSIZE dup(0)

RD_PATHMAX  EQU 128
rd_pathbuf  db  RD_PATHMAX dup(0)
rd_canbuf   db  RC_MAX dup(0)           ; rd_canon's answer
rc_n83      db  11 dup(0)               ; one component, 8.3, on its way there
rc_drv      db  0
rc_back     dw  2                       ; the drive's curdir_end
rc_root     dw  0                       ; in rd_canbuf, just past the root
rc_lim      dw  RC_LOCAL - 14           ; how long the name may grow
rd_fcb      db  11 dup(0)               ; the component being looked for
rd_deoff    dw  0                       ; where the found entry is, in buf
rd_delba    dd  0                       ; and which sector that was

rd_dfname   dw  0                       ; dir_find's arguments and state
rd_dfsec    dd  0
rd_dfclus   dd  0
rd_dfleft   dd  0
rd_dfroot   db  0
rd_dastart  dd  0

rp_drv      db  0
rp_vol      dw  0
rp_indir    db  0

; BUILD lptpad - pushes rd_sfts 1000h bytes up the image.  It was
; built to ask whether the DI in the Windows fault dump follows the
; SFT, and the answer was NO: DI stayed 9030 with the table moved
; 1000h and the image 4K larger, and every other register in that
; dump was identical too.  So DI is not the SFT, the fault does not
; depend on anything about this image, and "the scan faults at the
; table" is a dead reading.  Kept because it is the cheap way to ask
; the question again.  See NOTES.TXT, v0.53.
IFDEF RMDOS_PAD
rd_sftpad   db  1000h dup(0)
ENDIF
; THE BLOCK HEADER DOS PUTS IN FRONT OF A FILE TABLE, and the list of
; lists points here (LL_SFT, RMINFO.INC).  It was FFFF:FFFF there until
; v0.60 - the same unterminated chain head PM-DOS's own list of lists
; had until v0.47 - and the Windows kernel, having measured an entry by
; scanning for "CON ", WALKS the table from that pointer.  From
; FFFF:FFFF it walked whatever the machine had left at 10FFEFh: on
; 86Box that happened to end the walk, and under DOSBox-X Windows sat
; on its splash screen for good, 52h the last DOS call it ever made.
; The entries follow it with nothing in between - a walker takes them
; to start six bytes in.
;
; AND IT IS NOT THE LAST BLOCK: AN EMPTY ONE COMES AFTER IT (2026-10-06).
; MS-DOS always has two - the one in its data segment and the one
; SYSINIT builds for FILES= - and Windows for Workgroups 3.11's
; VSHARE.386 counts a file table in a way that only works if there are:
; it adds up a block, follows its next pointer, and stops when the
; first word of the block it arrived at is FFFFh.  From a DOS's last
; block that word is the next block's own link; from ours, alone, it
; was whatever FFFF:FFFF named.  With A20 off in the virtual machine
; that is 0FFEFh, and the 64K this DOS parks in is across it - so
; VSHARE read the end of its loop out of the middle of this image, the
; sector cache's directory, and stopped only if the cache had not yet
; used the entry it landed on: an unused one is all FFh.  Otherwise it
; walked memory for ever before WfW had put up its logo.  Which entry
; depended on where the park landed, to the paragraph: with IFSHLP.SYS
; in CONFIG.SYS (3.8K, and WfW's own Setup puts it there) Windows hung;
; a do-nothing driver of 2,784 bytes let it start and one of 2,816 did
; not; and with WINCACHE=OFF the directory is zeros and it never would.
;
; rd_sysinit links the two (RMINFO.INC) - only it knows the segment.
; DOSMGR takes the second block as the last one, as it takes SYSINIT's
; on a real DOS, and hangs its PerVMFiles= block off its link.
;
; AND THE SECOND BLOCK HAS AN ENTRY IN IT (2026-10-07).  It had none,
; and MS-DOS never builds a block with none - FILES=5 or fewer makes
; no second block at all (DOS6SRC bios/sysinit1.asm).  WfW's IFSMGR.386
; maps every entry in the chain into a table of its own at Device_Init,
; a block at a time, with the block's count in ECX and a LOOP: a count
; of 0 is four thousand million entries, and it wrote over ring-0
; memory until it faulted - Windows' silent halt with IFSHLP.SYS in
; CONFIG.SYS.  One entry is enough.  RMDOS never hands it out (sft_alloc
; looks in rd_sfts only), and its zero reference count says free to
; anything else that looks.
rd_sfthdr   dd  0FFFFFFFFh              ; next: rd_sftend, from rd_sysinit
            dw  SFT_COUNT               ; the entries in this one
rd_sfts     db  (SFT_COUNT * SFT_LEN) dup(0)
rd_sftend   dd  0FFFFFFFFh              ; the last block: no next
            dw  1                       ; and one entry, unused
            db  SFT_LEN dup(0)

; THE FCB TABLE THE LIST OF LISTS NAMES (LL_FCB): MS-DOS 5's default,
; FCBS=4,0, as SYSINIT builds it - each entry 'A' throughout but for
; a zero reference count and position.  RMDOS's own FCB calls never use
; it.  It is here because the same IFSMGR loop walks this chain after
; the file table's, and it was a header with a count of 0 in the block
; below 64K, where six bytes were all there was room for.  The default
; is what VSHARE.386 looks for: a table of four with none protected it
; swaps for one of sixteen of its own, as it does on DOS.
FCB_COUNT   EQU 4
rd_fcbtab   dd  0FFFFFFFFh              ; no next
            dw  FCB_COUNT
REPT FCB_COUNT
            dw  0                       ; SFT_REF: free
            db  (SFT_POS - 2) dup('A')
            dd  0                       ; SFT_POS
            db  (SFT_LEN - SFT_POS - 4) dup('A')
ENDM
