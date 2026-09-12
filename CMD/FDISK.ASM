;=====================================================================
; FDISK.ASM - FDISK.EXE, the partition editor.
;
;   FDISK                   report every disk's partition table
;   FDISK /L                ...and the logical drives inside the
;                           extended container, which is the same
;                           report with the chain walked
;
; WHAT MAKES THIS ONE DIFFERENT from the FDISK everyone remembers is
; that it can RESIZE, EXTEND and MOVE a partition instead of only
; creating and deleting them - and that it refuses to do so when the
; filesystem inside would not survive it.  That engine is ported from
; SAFEPART (S:\CPP\NEWDEV\SAFEPART, RELOCATE.CPP), which is 16-bit DOS
; C++; here it is 32-bit assembly running native on PM-DOS, which is
; the port Charlie asked for rather than a recompile.
;
; THE SECTORS COME THROUGH 21h/F1h AL=0Ch, 0Dh AND 0Eh, which went
; into the kernel for this program.  INT 25h and 26h cannot be used
; and the reason is worth stating once: they address a DRIVE LETTER,
; which is to say a volume, and the sector this program most needs -
; LBA 0, the partition table itself - lies outside every letter on the
; machine.  An API that starts by naming a volume cannot reach the
; thing volumes live on.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

MAX_DISKS       EQU     8               ; the ceiling on what F1h/0Ch may
                                        ; report.  It answers with the real
                                        ; number; this only bounds the loop

SEC_BYTES       EQU     512
FDC_REPORT      EQU     0
FDC_PLAN        EQU     1
FDC_MOVE        EQU     2
FDC_SIZE        EQU     3

RL_NAMELEN      EQU     56              ; one row of rl_stname, and the
                                        ; refusals are long because each
                                        ; one has to say what to do next

FDS_LOGICAL     EQU     0001h

FD_MAXARGS      EQU     6
FD_ARGLEN       EQU     16

;--- the FAT specification's own cluster-count boundaries ------------
; THE TYPE IS DECIDED HERE AND NOWHERE ELSE.  Not by the partition
; type byte, not by the string in the boot sector: a 32MB FAT12
; volume inside an 06h partition is legal and exists.
FAT12_MAXCLUS   EQU     4084            ; <= 4084 clusters is FAT12
FAT16_MAXCLUS   EQU     65524           ; 4085..65524 is FAT16
FAT12_MAXSPF    EQU     32              ; a FAT12 FAT is read whole, and
                                        ; twelve sectors is the most one
                                        ; can honestly need

;--- one computed FAT layout -----------------------------------------
FG_VALID        EQU     00h             ; dd - 0 when nothing fits
FG_BITS         EQU     04h             ; dd - 12 or 16
FG_SPC          EQU     08h             ; dd - sectors per cluster
FG_SPF          EQU     0Ch             ; dd - sectors per FAT
FG_CLUS         EQU     10h             ; dd - clusters
FG_DATA         EQU     14h             ; dd - data sectors
FG_ROOT         EQU     18h             ; dd - root directory sectors
FG_CAP          EQU     1Ch             ; dd - capacity in SECTORS.  Not in
                                        ; bytes: that overflows 32 bits at
                                        ; the top of FAT16's range, and the
                                        ; comparison it is for is exact in
                                        ; sectors anyway
FG_LEN          EQU     20h

;--- what the planner decided, and why -------------------------------
; The refusals are the feature; see rl_plan.
RL_OK           EQU     0
RL_NOCHANGE     EQU     1               ; the new range IS the old one
RL_NOTFAT       EQU     2               ; not a FAT volume this can resize
RL_READERR      EQU     3
RL_WRITEERR     EQU     4
RL_TOOSMALL     EQU     5               ; data is stored above the new end
RL_OVERLAP      EQU     6               ; it would run into a neighbour
RL_BOUNDS       EQU     7               ; off the disk, or over the MBR
RL_GEOMETRY     EQU     8               ; no legal FAT layout at that size
RL_NOTARMED     EQU     9               ; /W was not given
RL_REMAP        EQU     10              ; the cluster size would change
RL_FATTYPE      EQU     11              ; FAT12 and FAT16 would change places

OP_NONE         EQU     0
OP_MOVE         EQU     1
OP_GROW         EQU     2
OP_SHRINK       EQU     3
OP_MOVERS       EQU     4               ; move and resize at once



EBR_MAX         EQU     64              ; logical drives followed before the
                                        ; walk decides the chain is a loop.
                                        ; A corrupt "next" pointer that points
                                        ; backwards is a chain with no end, and
                                        ; a partition editor reading a damaged
                                        ; disk is the normal case, not the
                                        ; exceptional one

_FDK    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_FDK, DS:_FDK, ES:_FDK, SS:_FDK
        ORG     0

;=====================================================================
; D32 header.  The buffers live in the BSS: three sectors and the
; 64K block the relocator will copy through.
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET fd_entry>,<SEC_BYTES*3 + 65536>,4096

;=====================================================================
fd_entry:
        mov     eax,OFFSET img_end      ; carve up the BSS
        mov     [mbr_buf],eax
        add     eax,SEC_BYTES
        mov     [ebr_buf],eax
        add     eax,SEC_BYTES
        mov     [sec_buf],eax
        add     eax,SEC_BYTES
        mov     [cpy_buf],eax

;--- critical errors are ours ----------------------------------------
; The same argument DRIVES.EXE and FORMAT.COM both make: a program
; that reads disks for a living will meet one that will not read, and
; "Abort, Retry, Fail?" in the middle of a partition table is asking
; the user to arbitrate a failure this program is about to report
; properly - with an Abort that would take it down mid-report.
        mov     edx,OFFSET fd_crit
        mov     eax,2524h
        int     21h

        call    fd_parse
        jc      fd_usage

        cmp     byte ptr [fd_cmd],FDC_REPORT
        je      fd_report
        cmp     byte ptr [fd_cmd],FDC_PLAN
        je      fd_plan
        cmp     byte ptr [fd_cmd],FDC_MOVE
        je      fd_change
        cmp     byte ptr [fd_cmd],FDC_SIZE
        je      fd_change
        jmp     fd_usage

;---------------------------------------------------------------------
fd_usage:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     eax,4C01h
        int     21h

fd_done:
        mov     eax,4C00h
        int     21h

;--- an error that has already said what it was ----------------------
fd_fail:
        mov     eax,4C01h
        int     21h

;=====================================================================
; The command tail.
;
; Switches are gathered into a mask and arguments into a list, because
; the two are asked about at different times: a switch is a fact about
; the whole run and an argument belongs to whichever verb consumed it.
; Nothing here is positional, which is the one thing COPY's /A and /B
; taught this tree to be careful about - and no verb here has an
; argument whose meaning depends on where it was written.
;=====================================================================
fd_parse:
        mov     byte ptr [fd_cmd],FDC_REPORT
        mov     dword ptr [fd_sw],0
        mov     dword ptr [fd_nargs],0

        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
fp_next:
        or      ecx,ecx
        jz      fp_done
        mov     al,[esi]
        cmp     al,' '
        je      fp_skip
        cmp     al,9
        je      fp_skip
        cmp     al,13
        je      fp_done
        cmp     al,'/'
        je      fp_switch
        cmp     al,'-'
        je      fp_switch
        jmp     short fp_arg
fp_skip:
        inc     esi
        dec     ecx
        jmp     short fp_next

;--- a switch: one letter, folded up ---------------------------------
fp_switch:
        inc     esi
        dec     ecx
        jz      fp_bad
        mov     al,[esi]
        call    fd_upper
        cmp     al,'L'
        je      fp_sw_l
        cmp     al,'P'
        je      fp_sw_p
        cmp     al,'M'
        je      fp_sw_m
        cmp     al,'S'
        je      fp_sw_s
        cmp     al,'W'
        je      fp_sw_w
        cmp     al,'?'
        je      fp_bad
        cmp     al,'H'
        je      fp_bad
        jmp     short fp_bad
fp_sw_l:
        or      dword ptr [fd_sw],FDS_LOGICAL
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_p:
        mov     byte ptr [fd_cmd],FDC_PLAN
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_m:
        mov     byte ptr [fd_cmd],FDC_MOVE
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_s:
        mov     byte ptr [fd_cmd],FDC_SIZE
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_w:
        call    dk_arm
        inc     esi
        dec     ecx
        jmp     fp_next

;--- an argument: text up to the next separator ----------------------
fp_arg:
        mov     eax,[fd_nargs]
        cmp     eax,FD_MAXARGS
        jae     fp_bad
        imul    edi,eax,FD_ARGLEN
        add     edi,OFFSET fd_args
        inc     dword ptr [fd_nargs]
        xor     edx,edx                 ; EDX = characters taken so far
fp_argch:
        or      ecx,ecx
        jz      fp_argend
        mov     al,[esi]
        cmp     al,' '
        je      fp_argend
        cmp     al,9
        je      fp_argend
        cmp     al,13
        je      fp_argend
        cmp     edx,FD_ARGLEN - 1
        jae     fp_bad
        mov     [edi],al
        inc     edi
        inc     edx
        inc     esi
        dec     ecx
        jmp     short fp_argch
fp_argend:
        mov     byte ptr [edi],0
        jmp     fp_next

fp_done:
        clc
        ret
fp_bad:
        stc
        ret

;=====================================================================
; THE REPORT.
;
; One block per physical disk: what the BIOS says it is, then the four
; primary slots, then - with /L - the logical drives strung through
; the extended container.
;
; The two size columns are the point of the geometry line, and they
; are the same point DRIVES.EXE makes about type bytes and filesystems:
; CHS reach and true capacity are two different numbers that agree on
; a small disk and part company on a large one, and a partition table
; that is built from the wrong one is a table the BIOS cannot boot.
;=====================================================================
fd_report:
        call    vol_load                ; the drive letters, once

        mov     edi,OFFSET dsk_rec
        mov     edx,80h
        mov     eax,0F10Ch
        int     21h
        mov     [fd_ndisks],ecx         ; how many disks there can be,
                                        ; which the call answers whether or
                                        ; not the one asked about is there

        xor     ebx,ebx                 ; EBX = the disk index
fr_disk:
        cmp     ebx,[fd_ndisks]
        jae     fr_end
        cmp     ebx,MAX_DISKS
        jae     fr_end

        lea     edx,[ebx+80h]
        mov     edi,OFFSET dsk_rec
        mov     eax,0F10Ch
        int     21h
        jc      fr_nextdisk             ; a slot with no disk in it: the
                                        ; report is about disks that exist
        test    byte ptr [dsk_rec+DSK_FLAGS],DSKF_PRESENT
        jz      fr_nextdisk

        call    fr_head
        call    fr_table
        test    dword ptr [fd_sw],FDS_LOGICAL
        jz      fr_nextdisk
        call    fr_logicals
fr_nextdisk:
        inc     ebx
        jmp     short fr_disk
fr_end:
        call    crlf
        jmp     fd_done

;---------------------------------------------------------------------
; fr_head - the disk itself.  EBX = its index.
;---------------------------------------------------------------------
fr_head:
        pushad
        call    crlf
        mov     edx,OFFSET msg_disk
        call    puts
        mov     eax,ebx
        call    put1
        mov     edx,OFFSET msg_sp2
        call    puts

        movzx   eax,word ptr [dsk_rec+DSK_CYLS]
        call    put5
        mov     edx,OFFSET msg_cyls
        call    puts
        movzx   eax,word ptr [dsk_rec+DSK_HEADS]
        call    put3
        mov     edx,OFFSET msg_heads
        call    puts
        movzx   eax,word ptr [dsk_rec+DSK_SPT]
        call    put3
        mov     edx,OFFSET msg_spt
        call    puts

        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        call    sec2mb
        call    put6
        mov     edx,OFFSET msg_mb
        call    puts
        call    crlf

        ; The two numbers, and a word about them when they disagree.
        mov     edx,OFFSET msg_reach
        call    puts
        mov     eax,dword ptr [dsk_rec+DSK_CHS]
        call    put10
        mov     edx,OFFSET msg_ofcap
        call    puts
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        call    put10
        mov     edx,OFFSET msg_secs
        call    puts
        mov     eax,dword ptr [dsk_rec+DSK_CHS]
        cmp     eax,dword ptr [dsk_rec+DSK_TOTAL]
        jae     fr_hnowarn
        mov     edx,OFFSET msg_beyond
        call    puts
fr_hnowarn:
        call    crlf
        popad
        ret

;---------------------------------------------------------------------
; fr_table - the four primary slots of the disk in EBX.
;---------------------------------------------------------------------
fr_table:
        pushad
        mov     byte ptr [fd_mbrok],0
        push    ebx
        lea     eax,[ebx+80h]
        xor     ebx,ebx                 ; LBA 0
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_read
        pop     ebx
        jc      ft_unreadable

        mov     edi,[mbr_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     ft_notable
        mov     byte ptr [fd_mbrok],1

        mov     edx,OFFSET msg_thead
        call    puts

        xor     ebp,ebp                 ; EBP = the slot number, 0-3
ft_slot:
        imul    esi,ebp,PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF

        mov     eax,ebp
        inc     eax
        call    put2d                   ; the slot number, 1-based as
        call    sp1                     ; every FDISK has numbered them

        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      ft_empty

        mov     al,[esi+PE_BOOT]        ; active?
        cmp     al,80h
        jne     ft_notboot
        mov     al,'A'
        jmp     short ft_bootput
ft_notboot:
        mov     al,' '
ft_bootput:
        call    putc
        call    sp2

        movzx   eax,byte ptr [esi+PE_TYPE]
        call    put2                    ; the type byte...
        call    sp1
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    typename                ; ...and what it is called

        mov     eax,[esi+PE_START]
        call    put10
        mov     eax,[esi+PE_SECS]
        call    put12
        mov     eax,[esi+PE_SECS]
        call    sec2mb
        call    put7
        mov     edx,OFFSET msg_mb2
        call    puts

        ; Which letter, if any, the kernel gave it.  Matched on the
        ; disk and the START LBA and not on the slot number, because
        ; the volume table records where a volume IS: the slot it came
        ; out of is the kernel's own bookkeeping and a logical drive
        ; has no slot at all.
        mov     eax,[esi+PE_START]
        call    vol_letter
        jc      ft_noletter
        call    putc
        mov     al,':'
        call    putc
ft_noletter:
        call    crlf
        jmp     short ft_step

ft_empty:
        mov     edx,OFFSET msg_empty
        call    puts
ft_step:
        inc     ebp
        cmp     ebp,PART_MAX
        jb      ft_slot
        popad
        ret

ft_notable:
        mov     edx,OFFSET msg_notable
        call    puts
        popad
        ret
ft_unreadable:
        mov     edx,OFFSET msg_unread
        call    puts
        popad
        ret

;---------------------------------------------------------------------
; fr_logicals - walk the extended chain on the disk in EBX.
;
; THE TWO "START" FIELDS IN AN EXTENDED BOOT RECORD ARE RELATIVE TO
; DIFFERENT THINGS and that is the whole difficulty of this walk: slot
; 0's start is relative to the EBR that holds it, and slot 1's - the
; pointer to the next EBR - is relative to the FIRST extended record,
; the one the MBR named.  Getting that wrong produces a chain that
; walks a little further each time and eventually reads a sector that
; is nobody's, which is exactly what a partition editor must not do.
;---------------------------------------------------------------------
fr_logicals:
        pushad
        cmp     byte ptr [fd_mbrok],0
        je      fl_out

        ; Find the container.  The first one wins, because DOS only
        ; ever followed one chain per disk and a table with two is a
        ; table something else made.
        mov     esi,[mbr_buf]
        add     esi,PART_OFF
        xor     ebp,ebp
fl_find:
        mov     al,[esi+PE_TYPE]
        cmp     al,PT_EXTENDED
        je      fl_got
        cmp     al,PT_EXTENDEDL
        je      fl_got
        add     esi,PE_LEN
        inc     ebp
        cmp     ebp,PART_MAX
        jb      fl_find
        jmp     fl_out
fl_got:
        mov     eax,[esi+PE_START]
        mov     [fd_extbase],eax        ; what slot 1 is measured from
        mov     [fd_extcur],eax         ; and where the walk is now
        xor     ebp,ebp                 ; EBP = how many followed

fl_step:
        cmp     ebp,EBR_MAX
        jae     fl_toolong
        lea     eax,[ebx+80h]
        push    ebx
        mov     ebx,[fd_extcur]
        mov     ecx,1
        mov     edi,[ebr_buf]
        call    dk_read
        pop     ebx
        jc      fl_badebr
        mov     edi,[ebr_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     fl_badebr

        ; slot 0: the volume, if there is one
        mov     esi,edi
        add     esi,PART_OFF
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      fl_novol
        call    fl_showvol
fl_novol:
        ; slot 1: the next record, relative to the container's start
        mov     esi,[ebr_buf]
        add     esi,PART_OFF + PE_LEN
        mov     al,[esi+PE_TYPE]
        cmp     al,PT_EXTENDED
        je      fl_hasnext
        cmp     al,PT_EXTENDEDL
        je      fl_hasnext
        jmp     short fl_out
fl_hasnext:
        mov     eax,[esi+PE_START]
        or      eax,eax
        jz      fl_out
        add     eax,[fd_extbase]
        cmp     eax,[fd_extcur]
        je      fl_loop                 ; a record pointing at itself
        mov     [fd_extcur],eax
        inc     ebp
        jmp     fl_step

fl_out:
        popad
        ret
fl_badebr:
        mov     edx,OFFSET msg_badebr
        call    puts
        popad
        ret
fl_loop:
        mov     edx,OFFSET msg_chloop
        call    puts
        popad
        ret
fl_toolong:
        mov     edx,OFFSET msg_chlong
        call    puts
        popad
        ret

;--- one logical drive.  ESI -> its entry in the EBR at fd_extcur ----
fl_showvol:
        pushad
        mov     edx,OFFSET msg_lmark
        call    puts
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    put2
        call    sp1
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    typename

        mov     eax,[esi+PE_START]      ; relative to THIS record
        add     eax,[fd_extcur]
        push    eax
        call    put10
        mov     eax,[esi+PE_SECS]
        call    put12
        mov     eax,[esi+PE_SECS]
        call    sec2mb
        call    put7
        mov     edx,OFFSET msg_mb2
        call    puts
        pop     eax
        call    vol_letter
        jc      fs_noletter
        call    putc
        mov     al,':'
        call    putc
fs_noletter:
        call    crlf
        popad
        ret

;=====================================================================
; THE DISK LAYER.  What DISKIO.CPP is in SAFEPART, which reaches the
; BIOS through INT 13h; here it is 21h/F1h and the kernel's own block
; layer, so an IDE disk is read by the kernel's driver and not by the
; BIOS at all.
;
; dk_read  - AL = BIOS drive, EBX = LBA, ECX = sectors, EDI -> buffer
; dk_write - the same, and it refuses unless dk_arm has been called
;
; Both loop, because the kernel caps one call at ABS_MAXSEC sectors -
; the cap is about the size of a buffer it is being asked to trust,
; not about the size of a transfer, so splitting it here is the right
; place to answer it.
;=====================================================================
DK_CHUNK        EQU     128             ; ABS_MAXSEC, and the kernel refuses
                                        ; more.  Named again here rather than
                                        ; shared, because a program has no
                                        ; business being recompiled when the
                                        ; kernel raises its own limit - the
                                        ; loop below is correct for any value
                                        ; the kernel will accept

dk_read:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     [dk_drive],al
dr_loop:
        or      ecx,ecx
        jz      dr_ok
        mov     eax,ecx
        cmp     eax,DK_CHUNK
        jbe     dr_have
        mov     eax,DK_CHUNK
dr_have:
        push    ecx
        mov     ecx,eax
        movzx   edx,byte ptr [dk_drive]
        mov     eax,0F10Dh
        int     21h
        jc      dr_fail
        pop     ecx
        mov     eax,ecx
        cmp     eax,DK_CHUNK
        jbe     dr_last
        mov     eax,DK_CHUNK
dr_last:
        sub     ecx,eax
        add     ebx,eax
        shl     eax,9
        add     edi,eax
        jmp     short dr_loop
dr_ok:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
dr_fail:
        pop     ecx
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; dk_write - and the gate.
;
; SAFEPART's /WRITE flag, kept: nothing this program can be asked to
; do reaches a sector until dk_arm has been called, and dk_arm is
; called from exactly one place.  It is not a safety net for bugs -
; a bug that got as far as calling dk_write would call dk_arm too -
; it is a safety net for the USER, who can run every planning path
; and see every answer with the disk physically unable to change.
;---------------------------------------------------------------------
dk_arm:
        mov     byte ptr [dk_armed],1
        ret

dk_write:
        cmp     byte ptr [dk_armed],0
        je      dw_notarmed
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     [dk_drive],al
dw_loop:
        or      ecx,ecx
        jz      dw_ok
        mov     eax,ecx
        cmp     eax,DK_CHUNK
        jbe     dw_have
        mov     eax,DK_CHUNK
dw_have:
        push    ecx
        mov     ecx,eax
        movzx   edx,byte ptr [dk_drive]
        mov     eax,0F10Eh
        int     21h
        jc      dw_fail
        pop     ecx
        mov     eax,ecx
        cmp     eax,DK_CHUNK
        jbe     dw_last
        mov     eax,DK_CHUNK
dw_last:
        sub     ecx,eax
        add     ebx,eax
        shl     eax,9
        add     edi,eax
        jmp     short dw_loop
dw_ok:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
dw_fail:
        pop     ecx
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
dw_notarmed:
        stc
        ret

;=====================================================================
; THE VOLUME TABLE, read once so that the report can put a drive
; letter beside a partition.
;
; Matched on the disk and the start LBA rather than on a slot number:
; the kernel records where a volume IS, and a logical drive has no
; slot number to match against at all.
;=====================================================================
vol_load:
        pushad
        mov     dword ptr [vol_n],0
        xor     ebx,ebx
vl_one:
        cmp     ebx,LASTDRIVE
        jae     vl_done
        imul    edi,ebx,DRV_LEN
        add     edi,OFFSET vol_tbl
        mov     edx,ebx
        mov     eax,0F106h
        int     21h
        jc      vl_done
        mov     [vol_n],ecx
        inc     ebx
        jmp     short vl_one
vl_done:
        popad
        ret

;--- vol_letter - EAX = an absolute start LBA, EBX = the disk index.
;--- Returns AL = the letter, or CF set if no letter is on it.
vol_letter:
        push    ebx
        push    ecx
        push    edx
        push    esi
        mov     ecx,[vol_n]
        cmp     ecx,LASTDRIVE
        jbe     vt_bounded
        mov     ecx,LASTDRIVE
vt_bounded:
        lea     edx,[ebx+80h]           ; the BIOS number the table holds
        xor     ebx,ebx
vt_scan:
        cmp     ebx,ecx
        jae     vt_none
        imul    esi,ebx,DRV_LEN
        add     esi,OFFSET vol_tbl
        test    byte ptr [esi+DRV_FLAGS],DRVF_PRESENT
        jz      vt_step
        test    byte ptr [esi+DRV_FLAGS],DRVF_CDROM OR DRVF_DRIVER
        jnz     vt_step                 ; DRV_BIOS is not a disk on those
        cmp     byte ptr [esi+DRV_BIOS],dl
        jne     vt_step
        cmp     dword ptr [esi+DRV_START],eax
        jne     vt_step
        mov     eax,ebx
        add     al,'A'
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
vt_step:
        inc     ebx
        jmp     short vt_scan
vt_none:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret


;=====================================================================
; THE FAT VOLUME LAYER.  FATVOL.CPP, ported.
;
; It answers the two questions a resizer has to ask before it is
; allowed to touch anything:
;
;   1. How small can this volume get?  - by scanning the FAT for the
;      highest cluster still in use.  Not "how much is free": the
;      question is where the LAST occupied cluster is, because
;      everything below it has to stay inside the volume whether it
;      is in use or not.
;   2. For a target size, what layout is legal? - which is the
;      cluster-size chooser, and the FAT specification's rule that
;      the type is decided by the CLUSTER COUNT and by nothing else.
;      Not by the type byte in the partition table, and not by the
;      string in the boot sector: DRIVES.EXE exists to make that same
;      point and it is the same point here.
;
; ONE DELIBERATE DEVIATION FROM SAFEPART, and it is a behaviour
; change rather than a port artefact.  SAFEPART always PREFERS the
; smallest cluster size, which means almost every resize it plans
; lands in the cluster-remap regime - and RelocExecute refuses that
; regime, so the answer to "shrink this by 100MB" is usually "not
; supported" even though a perfectly safe same-layout shrink was
; available the whole time.  Here the preference is inverted: KEEP
; the volume's current cluster size whenever it is still legal and
; the data still fits, because that is the regime in which cluster
; numbers survive and no directory entry has to be rewritten.  The
; smallest-cluster geometry is still computed and still reported -
; as information about what a rebuild would achieve - but it is not
; what the executor is pointed at.
;
; A capacity is held in SECTORS and not in bytes, which is the other
; deviation.  cluster_count * sec_per_clus * bytes_per_sec overflows
; 32 bits at the top of FAT16's range (65524 clusters of 128 sectors
; is 4.29 thousand million bytes), and the comparison it is wanted
; for - "does what is in use still fit?" - is exact in sectors.
;=====================================================================

;--- fv_read - parse the boot sector of the volume at EBX on disk AL.
;--- CF set if it is not a FAT12/16 volume this program can work on.
fv_read:
        pushad
        mov     byte ptr [fv_valid],0
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        jc      fv_bad
        mov     esi,[sec_buf]
        cmp     word ptr [esi+1FEh],MBR_SIG
        jne     fv_bad

        movzx   eax,word ptr [esi+BPB_BPS]
        mov     [fv_bps],eax
        movzx   eax,byte ptr [esi+BPB_SPC]
        mov     [fv_spc],eax
        movzx   eax,word ptr [esi+BPB_RSVD]
        mov     [fv_rsvd],eax
        movzx   eax,byte ptr [esi+BPB_NFATS]
        mov     [fv_nfats],eax
        movzx   eax,word ptr [esi+BPB_ROOTENT]
        mov     [fv_rootent],eax
        movzx   eax,word ptr [esi+BPB_SPF16]
        mov     [fv_spf],eax
        movzx   eax,word ptr [esi+BPB_TOTSEC16]
        or      eax,eax
        jnz     fv_gottot
        mov     eax,[esi+BPB_TOTSEC32]  ; the 16-bit field is zero when the
fv_gottot:                              ; volume needs the 32-bit one
        mov     [fv_total],eax

        ; The three that make the rest of this meaningless if wrong.
        ; 512 bytes a sector is an assumption this whole program makes
        ; - the block layer speaks in 512s and so does the API - so a
        ; volume that says otherwise is refused rather than mis-read.
        cmp     dword ptr [fv_bps],SEC_BYTES
        jne     fv_bad
        cmp     dword ptr [fv_spc],0
        je      fv_bad
        cmp     dword ptr [fv_nfats],0
        je      fv_bad
        cmp     dword ptr [fv_spf],0
        je      fv_bad                  ; a FAT32 BPB shape: SPF16 is zero
                                        ; there and the real one is at 24h
        call    fv_derive
        cmp     dword ptr [fv_bits],32
        je      fv_bad                  ; out of scope, and honestly so:
                                        ; FAT32 has no root-directory region
                                        ; to relocate and its root is a chain
        mov     byte ptr [fv_valid],1
        popad
        clc
        ret
fv_bad:
        popad
        stc
        ret

;--- fv_derive - the fields that follow from the BPB primitives ------
fv_derive:
        pushad
        ; root directory sectors, rounded up
        mov     eax,[fv_rootent]
        shl     eax,5                   ; * 32 bytes an entry
        add     eax,[fv_bps]
        dec     eax
        xor     edx,edx
        div     dword ptr [fv_bps]
        mov     [fv_rootsec],eax

        ; data sectors = total - (reserved + FATs + root)
        mov     eax,[fv_nfats]
        imul    eax,[fv_spf]
        add     eax,[fv_rsvd]
        add     eax,[fv_rootsec]
        mov     ecx,eax                 ; ECX = everything that is not data
        mov     eax,[fv_total]
        sub     eax,ecx
        jbe     fv_nodata               ; a BPB whose overhead exceeds its
        mov     [fv_datasec],eax        ; own total is not one
        xor     edx,edx
        div     dword ptr [fv_spc]
        mov     [fv_clusters],eax
        jmp     short fv_class
fv_nodata:
        mov     dword ptr [fv_datasec],0
        mov     dword ptr [fv_clusters],0
fv_class:
        ; THE TYPE IS THE CLUSTER COUNT AND NOTHING ELSE.
        mov     eax,[fv_clusters]
        mov     dword ptr [fv_bits],12
        cmp     eax,FAT12_MAXCLUS
        jbe     fv_done
        mov     dword ptr [fv_bits],16
        cmp     eax,FAT16_MAXCLUS
        jbe     fv_done
        mov     dword ptr [fv_bits],32
fv_done:
        popad
        ret

;=====================================================================
; fv_scanused - the highest cluster in the FAT that is not free.
;
;   AL = BIOS drive, EBX = the volume's start LBA.
;   -> [fv_usedhigh] = that cluster (0 when the volume is empty)
;      [fv_usedsec]  = how many data sectors that accounts for
;
; NOT "how much is free".  A shrink has to keep every sector below the
; last occupied cluster, whatever the gaps in between contain - so the
; question is where the last one IS, and a volume that is one percent
; full with a file at the top cannot be shrunk at all.  That is not a
; defect in the answer; it is the answer.
;=====================================================================
fv_scanused:
        pushad
        mov     dword ptr [fv_usedhigh],0
        mov     dword ptr [fv_usedsec],0
        cmp     byte ptr [fv_valid],0
        je      fs_out

        mov     [fv_drive],al
        add     ebx,[fv_rsvd]           ; the first FAT
        mov     [fv_fatlba],ebx

        cmp     dword ptr [fv_bits],16
        je      fs_fat16
        jmp     fs_fat12

;--- FAT16: 256 two-byte entries a sector, and no entry straddles one,
;--- so it streams a sector at a time and never holds the whole FAT.
fs_fat16:
        xor     ebp,ebp                 ; EBP = the FAT sector
fs16_sec:
        cmp     ebp,[fv_spf]
        jae     fs_out
        mov     al,[fv_drive]
        mov     ebx,[fv_fatlba]
        add     ebx,ebp
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        jc      fs_out                  ; an unreadable FAT ends the scan with
                                        ; what it found: refusing to answer at
                                        ; all would be worse, and the caller
                                        ; checks fv_usedhigh against the volume
        mov     esi,[sec_buf]
        xor     ecx,ecx                 ; ECX = the entry within the sector
fs16_ent:
        mov     eax,ebp
        shl     eax,8                   ; sector * 256
        add     eax,ecx                 ; = the cluster number
        cmp     eax,2
        jb      fs16_step
        mov     edx,[fv_clusters]
        inc     edx
        cmp     eax,edx
        ja      fs_out                  ; past the last cluster the volume has
        cmp     word ptr [esi+ecx*2],0
        je      fs16_step
        mov     [fv_usedhigh],eax
fs16_step:
        inc     ecx
        cmp     ecx,256
        jb      fs16_ent
        inc     ebp
        jmp     short fs16_sec

;--- FAT12: entries straddle sector boundaries, so the whole FAT is
;--- read at once.  It fits: 4084 clusters at twelve bits is 6126
;--- bytes, twelve sectors.  The copy buffer is borrowed for it, which
;--- is safe because nothing is being copied at plan time.
fs_fat12:
        mov     ecx,[fv_spf]
        cmp     ecx,FAT12_MAXSPF
        jbe     fs12_size
        mov     ecx,FAT12_MAXSPF
fs12_size:
        mov     al,[fv_drive]
        mov     ebx,[fv_fatlba]
        mov     edi,[cpy_buf]
        call    dk_read
        jc      fs_out

        mov     ebp,2                   ; EBP = the cluster
fs12_one:
        mov     eax,[fv_clusters]
        inc     eax
        cmp     ebp,eax
        ja      fs_out
        mov     eax,ebp
        call    fat12_get
        or      eax,eax
        jz      fs12_step
        mov     [fv_usedhigh],ebp
fs12_step:
        inc     ebp
        jmp     short fs12_one

fs_out:
        ; and what that comes to in sectors.  Clusters are numbered
        ; from two, so a volume whose highest in-use cluster is N holds
        ; N-1 clusters' worth of data below it.
        mov     eax,[fv_usedhigh]
        cmp     eax,2
        jb      fs_none
        dec     eax
        imul    eax,[fv_spc]
        mov     [fv_usedsec],eax
fs_none:
        popad
        ret

;--- fat12_get - EAX = a cluster number, -> EAX = its FAT entry.
;--- The FAT is whole in cpy_buf.  A twelve-bit entry is at byte
;--- n + n/2, and which half of the pair it occupies depends on
;--- whether n is odd - which is the entire awkwardness of FAT12 and
;--- the reason the whole table is read rather than streamed.
fat12_get:
        push    ebx
        push    ecx
        push    edx
        mov     ecx,eax                 ; keep the cluster number
        mov     edx,eax
        shr     edx,1
        add     eax,edx                 ; EAX = n + n/2
        add     eax,[cpy_buf]
        movzx   ebx,word ptr [eax]      ; the pair, little-endian
        test    ecx,1
        jz      f12_even
        shr     ebx,4
        jmp     short f12_out
f12_even:
        and     ebx,0FFFh
f12_out:
        and     ebx,0FFFh
        mov     eax,ebx
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; fg_forspc - the FAT layout for ONE cluster size.
;
;   EAX = the volume size in sectors, CL = sectors per cluster,
;   EDI -> an FG_LEN geometry record.
;   CF set if that cluster size cannot form a legal FAT12/16 volume
;   of that size.
;
; The BPB choices that are NOT free - bytes per sector, how many FATs,
; how many root entries, how many reserved sectors - come from the
; volume being resized, in fv_*.  They are not free because changing
; any of them moves the data region, and moving the data region is
; the thing this whole engine exists to avoid doing casually.
;
; The FAT is sized as FAT16 first and the result is then CLASSIFIED,
; because the type follows from the cluster count and the cluster
; count follows from the FAT size.  When the answer comes out FAT12
; the FAT is sized again with twelve-bit entries, because a FAT12
; volume carrying a FAT16-sized FAT is legal, wasteful, and a lie
; about its own layout.
;=====================================================================
fg_forspc:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     [fgw_total],eax
        movzx   eax,cl
        mov     [fgw_spc],eax
        mov     ebp,edi                 ; EBP -> the record

        ; clear it, so a refusal leaves a record that says "invalid"
        ; rather than the last plan's answer
        push    edi
        xor     eax,eax
        mov     ecx,FG_LEN / 4
        rep     stosd
        pop     edi

        cmp     dword ptr [fv_bps],0
        je      fg_no
        cmp     dword ptr [fgw_spc],0
        je      fg_no

        ; the root directory is fixed and comes along unchanged
        mov     eax,[fv_rootent]
        shl     eax,5
        add     eax,[fv_bps]
        dec     eax
        xor     edx,edx
        div     dword ptr [fv_bps]
        mov     [fgw_rootsec],eax

        mov     eax,16
        call    fg_estspf               ; -> EAX = sectors per FAT
        or      eax,eax
        jz      fg_no
        mov     [fgw_spf],eax

        ; is there anything left after the overhead?
        call    fg_overhead             ; -> EAX = reserved + FATs + root
        cmp     eax,[fgw_total]
        jae     fg_no
        mov     ecx,eax
        mov     eax,[fgw_total]
        sub     eax,ecx
        mov     [fgw_data],eax
        xor     edx,edx
        div     dword ptr [fgw_spc]
        mov     [fgw_clus],eax
        cmp     eax,FAT16_MAXCLUS
        ja      fg_no                   ; clusters too small for this size

        cmp     eax,FAT12_MAXCLUS
        ja      fg_is16

        ; It is FAT12, so size the FAT again honestly and re-derive.
        ; A FAT12 volume carrying a FAT16-sized FAT is legal, wasteful,
        ; and a lie about its own layout.
        mov     dword ptr [fgw_bits],12
        mov     eax,12
        call    fg_estspf
        or      eax,eax
        jz      fg_no
        mov     [fgw_spf],eax
        call    fg_derive
        jc      fg_no
        jmp     short fg_settle
fg_is16:
        mov     dword ptr [fgw_bits],16

;--- and now make it settle ------------------------------------------
; The estimate can land one sector short of what its own answer needs
; to describe, and a FAT with no room for its last entries is not
; waste, it is corruption.  So: ask how many sectors the derived
; cluster count REALLY needs, and if the FAT is smaller than that,
; grow it to exactly that and derive again.  Growing shrinks the
; cluster count, which shrinks the requirement, so this cannot
; oscillate - but it is bounded anyway, because a loop that decides
; how big to make a FAT is not a loop to leave unbounded.
fg_settle:
        mov     dword ptr [fgw_iter],4
fg_stloop:
        call    fg_fatneed              ; -> EAX = sectors really needed
        cmp     eax,[fgw_spf]
        jbe     fg_fill                 ; it fits
        mov     [fgw_spf],eax
        call    fg_derive
        jc      fg_no
        dec     dword ptr [fgw_iter]
        jnz     fg_stloop
        jmp     fg_no                   ; it would not settle: refuse rather
                                        ; than write a FAT nobody checked
fg_fill:
        cmp     dword ptr [fgw_clus],0
        je      fg_no                   ; a volume with no clusters is not one

        mov     dword ptr [ebp+FG_VALID],1
        mov     eax,[fgw_bits]
        mov     [ebp+FG_BITS],eax
        mov     eax,[fgw_spc]
        mov     [ebp+FG_SPC],eax
        mov     eax,[fgw_spf]
        mov     [ebp+FG_SPF],eax
        mov     eax,[fgw_clus]
        mov     [ebp+FG_CLUS],eax
        mov     eax,[fgw_data]
        mov     [ebp+FG_DATA],eax
        mov     eax,[fgw_rootsec]
        mov     [ebp+FG_ROOT],eax
        mov     eax,[fgw_clus]           ; capacity, in SECTORS
        imul    eax,[fgw_spc]
        mov     [ebp+FG_CAP],eax
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
fg_no:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;--- fg_derive - data sectors and cluster count from the FAT size that
;--- is in fgw_spf now.  CF set if the overhead has eaten the volume.
fg_derive:
        push    edx
        call    fg_overhead
        cmp     eax,[fgw_total]
        jae     fgv_bad
        push    ecx
        mov     ecx,eax
        mov     eax,[fgw_total]
        sub     eax,ecx
        mov     [fgw_data],eax
        xor     edx,edx
        div     dword ptr [fgw_spc]
        mov     [fgw_clus],eax
        pop     ecx
        pop     edx
        clc
        ret
fgv_bad:
        pop     edx
        stc
        ret

;--- fg_fatneed - how many sectors a FAT must be to hold an entry for
;--- every cluster in fgw_clus.  -> EAX.
;---
;--- CLUSTERS PLUS TWO, because entries 0 and 1 are not clusters: entry
;--- 0 carries the media byte and entry 1 the end-of-chain marker, and
;--- a FAT sized for the cluster count alone is two entries short at
;--- the top - which is the last two clusters on the volume, the ones
;--- a nearly-full disk is about to use.
fg_fatneed:
        push    ecx
        push    edx
        mov     eax,[fgw_clus]
        add     eax,2
        cmp     dword ptr [fgw_bits],12
        je      fn_12
        shl     eax,1                   ; two bytes an entry
        jmp     short fn_secs
fn_12:
        lea     eax,[eax*2+eax]         ; three halves: (n*3 + 1) / 2
        inc     eax
        shr     eax,1
fn_secs:
        add     eax,SEC_BYTES - 1
        mov     ecx,SEC_BYTES
        xor     edx,edx
        div     ecx
        pop     edx
        pop     ecx
        ret

;--- fg_overhead - reserved + all the FATs + the root, in sectors ----
fg_overhead:
        push    edx
        mov     eax,[fv_nfats]
        imul    eax,[fgw_spf]
        add     eax,[fv_rsvd]
        add     eax,[fgw_rootsec]
        pop     edx
        ret

;--- fg_estspf - sectors per FAT.  EAX = 12 or 16 on the way in,
;--- sectors per FAT on the way out, or zero when the sum cannot be
;--- formed at all.
;---
;--- TWO THINGS SAFEPART GETS WRONG HERE, and this port found them by
;--- shrinking a real volume and reading the number back.
;---
;--- The FAT specification's own formula is
;---
;---     TmpVal1 = DskSize - (ResvdSecCnt + RootDirSectors)
;---     TmpVal2 = (256 * SecPerClus) + NumFATs
;---     if (FAT32) TmpVal2 = TmpVal2 / 2
;---     FATSz = ceil( TmpVal1 / TmpVal2 )
;---
;--- and the halving is FAT32's, not FAT16's: the 256 is already
;--- 512/2, one sector's worth of two-byte entries.  SAFEPART halves
;--- it for FAT16, which doubles the FAT.  On a 40000-sector volume
;--- that is 156 sectors where 78 is right - and the FAT growing
;--- rather than shrinking is what made a SHRINK move its data region
;--- to the RIGHT, which is how the bug was noticed at all.
;---
;--- FAT12 wants the same derivation with 1.5-byte entries.  Setting
;--- FATSz*(512/1.5)*spc = t1 - nfats*FATSz gives
;--- FATSz = 3*t1 / (1024*spc + 3*nfats), which is the form below.
;--- SAFEPART uses (256*spc + nfats)*2/3, about half the divisor, so
;--- its FAT12 FAT comes out roughly twice the size as well.
;---
;--- Neither error is UNSAFE - an oversized FAT is legal and the
;--- cluster count derived from it is honest, so the volume that
;--- results is consistent - but it wastes space and it moves data
;--- that did not need moving.
;---
;--- The guard SAFEPART also does not have is the first one below: a
;--- target smaller than the volume's own fixed overhead makes t1
;--- negative, and in unsigned arithmetic negative is enormous - so
;--- the estimate comes back as a FAT of hundreds of thousands of
;--- sectors and every check downstream passes it.
fg_estspf:
        push    ebx
        push    ecx
        push    edx
        mov     ebx,eax                 ; EBX = 12 or 16

        mov     eax,[fv_rsvd]
        add     eax,[fgw_rootsec]
        cmp     eax,[fgw_total]
        jae     fg_est0
        mov     ecx,eax
        mov     eax,[fgw_total]
        sub     eax,ecx                 ; EAX = t1

        push    eax
        cmp     ebx,16
        jne     fg_est12
        mov     eax,[fgw_spc]
        shl     eax,8                   ; 256 * spc + FATs, and NOTHING
        add     eax,[fv_nfats]          ; else - see the derivation above
        jmp     short fg_esthave
fg_est12:
        mov     eax,[fgw_spc]
        shl     eax,10                  ; (1024 * spc + 3 * FATs) / 3
        mov     ecx,[fv_nfats]
        lea     ecx,[ecx*2+ecx]
        add     eax,ecx
        mov     ecx,3
        xor     edx,edx
        div     ecx
fg_esthave:
        mov     ecx,eax                 ; ECX = t2
        pop     eax                     ; EAX = t1
        or      ecx,ecx
        jz      fg_est0
        add     eax,ecx
        dec     eax                     ; round up
        xor     edx,edx
        div     ecx
        pop     edx
        pop     ecx
        pop     ebx
        ret
fg_est0:
        xor     eax,eax
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; fg_smallest - the smallest cluster size that yields a legal volume
;               of EAX sectors.  EDI -> the geometry record.
;
; Walked from one upwards, so the first that works IS the smallest -
; which is the rule the FAT specification states for a tool that
; resizes a volume, and the geometry this program REPORTS even though
; it is not the geometry it executes.  See the deviation note above.
;=====================================================================
fg_smallest:
        push    eax
        push    ecx
        mov     [fgw_want],eax
        mov     ecx,1
fgs_try:
        mov     eax,[fgw_want]
        call    fg_forspc
        jnc     fgs_got
        shl     ecx,1
        cmp     ecx,128
        jbe     fgs_try
        pop     ecx
        pop     eax
        stc
        ret
fgs_got:
        pop     ecx
        pop     eax
        clc
        ret


;=====================================================================
; THE PLANNER.  RelocPlan(), ported.
;
; It decides whether a proposed (start, length) is possible and says
; why when it is not, and it does that WITHOUT WRITING ANYTHING - so
; every answer this program gives can be got out of it with the disk
; physically unable to change.  That separation is the whole design:
; planning is a pure function of what is on the disk, and the executor
; is a dumb transcription of a plan that has already been approved.
;
; THE REFUSALS ARE THE FEATURE.  Charlie's requirement was that it
; resize, extend and move "as long as none of those violate the rules
; of the given filesystem", and these are those rules:
;
;   TOOSMALL   something is stored above the proposed new end.  Not
;              "the volume is too full" - the question is where the
;              LAST occupied cluster is, so a volume one percent full
;              with one file at the top cannot shrink at all
;   REMAP      the new size cannot be expressed at this cluster size,
;              so every cluster would be renumbered and every
;              directory entry rewritten
;   FATTYPE    the cluster count crosses FAT12/FAT16, which is the
;              same rebuild by another name
;   OVERLAP    it would run into another partition
;   BOUNDS     it would run off the disk, or over the MBR
;=====================================================================
rl_plan:
        pushad
        mov     dword ptr [pl_status],RL_OK
        mov     dword ptr [pl_op],OP_NONE
        mov     byte ptr [pl_same],0
        mov     byte ptr [pl_crossbl],0
        mov     byte ptr [pl_keepok],0

        imul    esi,dword ptr [pl_idx],PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        mov     eax,[esi+PE_START]
        mov     [pl_ostart],eax
        mov     eax,[esi+PE_SECS]
        mov     [pl_olen],eax
        mov     byte ptr [pl_boot],0
        cmp     byte ptr [esi+PE_BOOT],80h
        jne     rp_notboot
        mov     byte ptr [pl_boot],1
rp_notboot:
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    rl_isfat
        jc      rp_notfat

;--- the volume as it stands -----------------------------------------
        mov     al,[pl_drive]
        mov     ebx,[pl_ostart]
        call    fv_read
        jc      rp_notfat
        mov     al,[pl_drive]
        mov     ebx,[pl_ostart]
        call    fv_scanused

;--- nothing asked for is not an error -------------------------------
        mov     eax,[pl_nstart]
        cmp     eax,[pl_ostart]
        jne     rp_changed
        mov     eax,[pl_nlen]
        cmp     eax,[pl_olen]
        jne     rp_changed
        mov     dword ptr [pl_status],RL_NOCHANGE
        jmp     rp_out
rp_changed:
        call    rl_classify


;--- bounds ----------------------------------------------------------
; Sector zero is the master boot record and belongs to nobody, so a
; partition may not start there however the arithmetic works out.
        cmp     dword ptr [pl_nstart],1
        jb      rp_bounds
        cmp     dword ptr [pl_nlen],0
        je      rp_bounds
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]
        jc      rp_bounds               ; a length that wraps 4GB is not one
        cmp     eax,[phy_total]
        ja      rp_bounds

;--- overlap with any other occupied slot ----------------------------
        xor     ebp,ebp
rp_ovl:
        cmp     ebp,PART_MAX
        jae     rp_noovl
        cmp     ebp,[pl_idx]
        je      rp_ovlstep
        imul    esi,ebp,PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      rp_ovlstep
        ; they overlap unless one ends at or before the other starts
        mov     eax,[esi+PE_START]
        add     eax,[esi+PE_SECS]       ; the other's end
        mov     edx,[pl_nstart]
        cmp     edx,eax
        jae     rp_ovlstep              ; we start at or after its end
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]           ; our end
        mov     edx,[esi+PE_START]
        cmp     edx,eax
        jae     rp_ovlstep              ; it starts at or after our end
        mov     dword ptr [pl_status],RL_OVERLAP
        jmp     rp_out
rp_ovlstep:
        inc     ebp
        jmp     short rp_ovl
rp_noovl:

;--- the geometry the specification would prefer, for the report -----
        mov     eax,[pl_nlen]
        mov     edi,OFFSET fg_ideal
        call    fg_smallest

;--- and the geometry this program will actually execute -------------
; THE CURRENT CLUSTER SIZE, kept - see the deviation note in the FAT
; volume layer.  It is the regime in which cluster NUMBERS survive,
; which is what makes the operation non-destructive: no file's data
; moves relative to the data region and no directory entry has to be
; touched.
        mov     eax,[pl_nlen]
        mov     ecx,[fv_spc]
        mov     edi,OFFSET fg_keep
        call    fg_forspc
        jc      rp_remap
        cmp     dword ptr [fg_keep+FG_VALID],0
        je      rp_remap
        mov     eax,dword ptr [fg_keep+FG_BITS]
        cmp     eax,[fv_bits]
        jne     rp_fattype
        mov     eax,dword ptr [fg_keep+FG_CAP]
        cmp     eax,[fv_usedsec]
        jb      rp_toosmall

        mov     byte ptr [pl_keepok],1
        mov     byte ptr [pl_same],1
        mov     esi,OFFSET fg_keep      ; exec = keep
        mov     edi,OFFSET fg_exec
        mov     ecx,FG_LEN / 4
        rep     movsd

;--- and does it put a bootable volume out of the BIOS's reach? ------
; Not fatal, and not this program's decision: a partition past the
; 1024-cylinder line is perfectly legal and perfectly readable, it
; simply cannot be booted from by a BIOS that has only CHS.  So it is
; reported and the user decides.
rp_boot:
        cmp     byte ptr [pl_boot],0
        je      rp_ok
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]
        cmp     eax,[phy_chs]
        jbe     rp_ok
        mov     byte ptr [pl_crossbl],1
rp_ok:
        mov     dword ptr [pl_status],RL_OK
        jmp     short rp_out

rp_notfat:
        mov     dword ptr [pl_status],RL_NOTFAT
        jmp     short rp_out
rp_bounds:
        mov     dword ptr [pl_status],RL_BOUNDS
        jmp     short rp_out
rp_remap:
        mov     dword ptr [pl_status],RL_REMAP
        jmp     short rp_out
rp_fattype:
        mov     dword ptr [pl_status],RL_FATTYPE
        jmp     short rp_out
rp_toosmall:
        mov     dword ptr [pl_status],RL_TOOSMALL
rp_out:
        popad
        ret

;--- rl_classify - what was ASKED for, which is a fact about the two
;--- ranges and needs no disk at all.  Called before the first check,
;--- so that a refusal can still name the operation it refused.
rl_classify:
        push    eax
        mov     eax,[pl_nstart]
        cmp     eax,[pl_ostart]
        je      rc_samestart
        mov     eax,[pl_nlen]
        cmp     eax,[pl_olen]
        je      rc_ismove
        mov     dword ptr [pl_op],OP_MOVERS
        pop     eax
        ret
rc_ismove:
        mov     dword ptr [pl_op],OP_MOVE
        pop     eax
        ret
rc_samestart:
        mov     eax,[pl_nlen]
        cmp     eax,[pl_olen]
        ja      rc_isgrow
        mov     dword ptr [pl_op],OP_SHRINK
        pop     eax
        ret
rc_isgrow:
        mov     dword ptr [pl_op],OP_GROW
        pop     eax
        ret

;--- rl_isfat - AL = a type byte.  CF clear if it names a FAT volume
;--- this program is prepared to resize.  The hidden spellings are
;--- deliberately NOT here: hiding a volume is a request, and a tool
;--- that resizes one anyway has ignored it.
rl_isfat:
        cmp     al,PT_FAT12
        je      rif_yes
        cmp     al,PT_FAT16S
        je      rif_yes
        cmp     al,PT_FAT16B
        je      rif_yes
        cmp     al,PT_FAT16L
        je      rif_yes
        stc
        ret
rif_yes:
        clc
        ret

;=====================================================================
; THE LIMITS.  What the /P report is really for: not "here is the
; BPB" but "here is how far this can go", which is the question
; somebody about to drag a partition boundary actually has.
;
; rl_ccfor - EAX = a candidate volume size in sectors
;            -> EAX = how many clusters it would have at THIS volume's
;               cluster size and FAT width.  Zero when the size cannot
;               hold even its own overhead.
;
; The FAT width is held fixed rather than reclassified, because these
; limits are about the same-layout regime: a size whose cluster count
; crosses into the other width is not a size this volume can take,
; and reporting it would be reporting a plan that rl_plan refuses.
;=====================================================================
rl_ccfor:
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     [fgw_total],eax
        mov     eax,[fv_spc]
        mov     [fgw_spc],eax
        mov     eax,[fv_rootent]
        shl     eax,5
        add     eax,[fv_bps]
        dec     eax
        xor     edx,edx
        div     dword ptr [fv_bps]
        mov     [fgw_rootsec],eax
        mov     eax,[fv_bits]
        call    fg_estspf
        or      eax,eax
        jz      rc_zero
        mov     [fgw_spf],eax
        call    fg_overhead
        cmp     eax,[fgw_total]
        jae     rc_zero
        mov     ecx,eax
        mov     eax,[fgw_total]
        sub     eax,ecx
        xor     edx,edx
        div     dword ptr [fgw_spc]
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        ret
rc_zero:
        xor     eax,eax
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- rl_minsize - the smallest volume that still holds what is stored.
;--- EBX = the largest size worth considering (the search's ceiling).
;--- -> EAX, or zero when even the ceiling will not do.
;---
;--- BINARY SEARCH, and it is allowed to be one because the cluster
;--- count is monotonic in the volume size: a bigger volume never has
;--- fewer clusters.  The predicate rl_plan applies is NOT monotonic
;--- (growing a FAT12 volume far enough makes it a FAT16 one, which
;--- rl_plan refuses), which is why the search is on the CLUSTER COUNT
;--- and the width is pinned - searching the predicate directly would
;--- converge on the wrong side of that boundary.
rl_minsize:
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     eax,[fv_usedsec]
        xor     edx,edx
        div     dword ptr [fv_spc]      ; clusters needed for what is stored
        or      edx,edx
        jz      rms_exact
        inc     eax                     ; round up: a part-used cluster is used
rms_exact:
        cmp     eax,1
        ja      rms_have
        mov     eax,1                   ; a volume must have at least one
rms_have:
        mov     [rl_needcc],eax

        mov     esi,1                   ; lo
        mov     edi,ebx                 ; hi
rms_loop:
        cmp     esi,edi
        jae     rms_done
        mov     eax,edi
        sub     eax,esi
        shr     eax,1
        add     eax,esi                 ; mid = lo + (hi-lo)/2
        push    eax
        call    rl_ccfor
        mov     ecx,eax
        pop     eax
        cmp     ecx,[rl_needcc]
        jb      rms_up
        mov     edi,eax                 ; hi = mid
        jmp     short rms_loop
rms_up:
        lea     esi,[eax+1]             ; lo = mid + 1
        jmp     short rms_loop
rms_done:
        mov     eax,esi
        push    eax
        call    rl_ccfor
        mov     ecx,eax
        pop     eax
        cmp     ecx,[rl_needcc]
        jae     rms_out
        xor     eax,eax                 ; even the ceiling was not enough
rms_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        ret

;--- rl_maxsize - the largest volume this cluster size can still
;--- express without changing the FAT width.  EBX = the ceiling that
;--- the disk and the neighbouring partitions impose.  -> EAX.
rl_maxsize:
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     eax,FAT12_MAXCLUS
        cmp     dword ptr [fv_bits],12
        je      rmx_ceil
        mov     eax,FAT16_MAXCLUS
rmx_ceil:
        mov     [rl_maxcc],eax

        mov     esi,1                   ; lo
        mov     edi,ebx                 ; hi
rmx_loop:
        cmp     esi,edi
        jae     rmx_done
        mov     eax,edi
        sub     eax,esi
        inc     eax
        shr     eax,1
        add     eax,esi                 ; mid = lo + (hi-lo+1)/2
        push    eax
        call    rl_ccfor
        mov     ecx,eax
        pop     eax
        cmp     ecx,[rl_maxcc]
        ja      rmx_down
        mov     esi,eax                 ; lo = mid
        jmp     short rmx_loop
rmx_down:
        lea     edi,[eax-1]             ; hi = mid - 1
        jmp     short rmx_loop
rmx_done:
        mov     eax,esi
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        ret

;--- rl_room - how far the partition in [pl_idx] could extend before
;--- it met something.  -> EAX = the largest length it could have at
;--- its current start.
rl_room:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    ebp
        mov     ebx,[phy_total]         ; the end of the disk, until something
                                        ; nearer turns up
        xor     ebp,ebp
rr_scan:
        cmp     ebp,PART_MAX
        jae     rr_done
        cmp     ebp,[pl_idx]
        je      rr_step
        imul    esi,ebp,PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      rr_step
        mov     eax,[esi+PE_START]
        cmp     eax,[pl_ostart]
        jbe     rr_step                 ; it is behind us, so it is not a wall
        cmp     eax,ebx
        jae     rr_step
        mov     ebx,eax                 ; the nearest thing in front
rr_step:
        inc     ebp
        jmp     short rr_scan
rr_done:
        mov     eax,ebx
        sub     eax,[pl_ostart]
        pop     ebp
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret


;=====================================================================
; /P - what one partition is, and how far it could move.
;
; THE LIMITS ARE THE REPORT.  A BPB dump is a thing anyone can get out
; of a sector editor; the question somebody about to change a
; partition boundary actually has is "how far can I take this, and
; what stops me" - so that is what this prints, and each of the three
; numbers has a different reason behind it.
;=====================================================================
fd_plan:
        cmp     dword ptr [fd_nargs],1
        jb      fdp_usage
        call    fd_getdp
        jc      fdp_usage

        movzx   edx,byte ptr [pl_drive]
        mov     edi,OFFSET dsk_rec
        mov     eax,0F10Ch
        int     21h
        jc      fdp_nodisk
        test    byte ptr [dsk_rec+DSK_FLAGS],DSKF_PRESENT
        jz      fdp_nodisk
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        mov     [phy_total],eax
        mov     eax,dword ptr [dsk_rec+DSK_CHS]
        mov     [phy_chs],eax

        mov     al,[pl_drive]
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_read
        jc      fdp_unread
        mov     edi,[mbr_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     fdp_notable

;--- plan a change of nothing, which is how the volume gets parsed ---
; rl_plan reads the BPB and scans the FAT before it looks at what was
; asked for, so asking it for no change at all is the cheapest way to
; get the volume's own state without a second path that could drift
; out of agreement with the one the executor uses.
        imul    esi,dword ptr [pl_idx],PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        mov     eax,[esi+PE_START]
        mov     [pl_nstart],eax
        mov     eax,[esi+PE_SECS]
        mov     [pl_nlen],eax
        call    rl_plan
        cmp     dword ptr [pl_status],RL_NOTFAT
        je      fdp_notfat

;--- the heading -----------------------------------------------------
        call    crlf
        mov     edx,OFFSET msg_disk
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_part
        call    puts
        mov     eax,[pl_idx]
        inc     eax
        call    put1
        mov     edx,OFFSET msg_sp3
        call    puts
        imul    esi,dword ptr [pl_idx],PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    put2
        call    sp1
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    typename
        mov     edx,OFFSET msg_at
        call    puts
        mov     eax,[pl_ostart]
        call    put1
        mov     edx,OFFSET msg_comma
        call    puts
        mov     eax,[pl_olen]
        call    put1
        mov     edx,OFFSET msg_secmb
        call    puts
        mov     eax,[pl_olen]
        call    sec2mb
        call    put1
        mov     edx,OFFSET msg_mbclose
        call    puts

;--- the volume ------------------------------------------------------
        mov     edx,OFFSET msg_vol
        call    puts
        mov     edx,OFFSET msg_fat
        call    puts
        mov     eax,[fv_bits]
        call    put2d
        mov     edx,OFFSET msg_sp2
        call    puts
        mov     eax,[fv_spc]
        call    put3
        mov     edx,OFFSET msg_spclus
        call    puts
        mov     eax,[fv_nfats]
        call    put1
        mov     edx,OFFSET msg_fats
        call    puts
        mov     eax,[fv_rootent]
        call    put5
        mov     edx,OFFSET msg_rootent
        call    puts
        mov     eax,[fv_spf]
        call    put5
        mov     edx,OFFSET msg_spfat
        call    puts

        mov     edx,OFFSET msg_vol
        call    puts
        mov     eax,[fv_clusters]
        call    put8
        mov     edx,OFFSET msg_clusters
        call    puts
        mov     eax,[fv_datasec]
        call    put8
        mov     edx,OFFSET msg_datasec
        call    puts

;--- what is in the way ----------------------------------------------
        mov     edx,OFFSET msg_inuse
        call    puts
        cmp     dword ptr [fv_usedhigh],0
        jne     fdp_used
        mov     edx,OFFSET msg_empty2
        call    puts
        jmp     short fdp_limits
fdp_used:
        mov     eax,[fv_usedhigh]
        call    put8
        mov     edx,OFFSET msg_upto
        call    puts
        mov     eax,[fv_usedsec]
        call    put8
        mov     edx,OFFSET msg_secmb
        call    puts
        mov     eax,[fv_usedsec]
        call    sec2mb
        call    put1
        mov     edx,OFFSET msg_mbclose
        call    puts

;--- and how far it can go -------------------------------------------
fdp_limits:
        cmp     byte ptr [fv_valid],0
        je      fdp_done

        call    rl_room
        mov     [pl_room],eax

        mov     edx,OFFSET msg_room
        call    puts
        mov     eax,[pl_room]
        call    put10
        call    fdp_mb

        mov     ebx,[pl_room]
        call    rl_minsize
        mov     edx,OFFSET msg_shrink
        call    puts
        or      eax,eax
        jz      fdp_noshrink
        push    eax
        call    put10
        pop     eax
        call    fdp_mb
        jmp     short fdp_grow
fdp_noshrink:
        mov     edx,OFFSET msg_nolimit
        call    puts

fdp_grow:
        mov     ebx,[pl_room]
        call    rl_maxsize
        mov     edx,OFFSET msg_grow
        call    puts
        push    eax
        call    put10
        pop     eax
        call    fdp_mb

;--- and what the specification would have preferred -----------------
; Computed HERE and not taken from the plan, because the plan this
; report asks for is a plan to change nothing - and rl_plan answers
; that one before it has worked out any geometry at all.  Reading a
; field the planner had no reason to fill is how a report ends up
; quietly printing the last run's numbers.
        mov     eax,[pl_olen]
        mov     edi,OFFSET fg_ideal
        call    fg_smallest
        cmp     dword ptr [fg_ideal+FG_VALID],0
        je      fdp_done
        mov     eax,dword ptr [fg_ideal+FG_SPC]
        cmp     eax,[fv_spc]
        je      fdp_done                ; it already has it
        mov     edx,OFFSET msg_ideal
        call    puts
        mov     eax,dword ptr [fg_ideal+FG_SPC]
        call    put1
        mov     edx,OFFSET msg_idealto
        call    puts
        mov     edx,OFFSET msg_fat
        call    puts
        mov     eax,dword ptr [fg_ideal+FG_BITS]
        call    put2d
        mov     edx,OFFSET msg_idealend
        call    puts

fdp_done:
        call    crlf
        jmp     fd_done

fdp_mb:
        push    eax
        push    edx
        mov     edx,OFFSET msg_secmb
        call    puts
        call    sec2mb
        call    put1
        mov     edx,OFFSET msg_mbclose
        call    puts
        pop     edx
        pop     eax
        ret

fdp_usage:
        jmp     fd_usage
fdp_nodisk:
        mov     edx,OFFSET msg_nodisk
        call    puts
        jmp     fd_fail
fdp_unread:
        mov     edx,OFFSET msg_unread
        call    puts
        jmp     fd_fail
fdp_notable:
        mov     edx,OFFSET msg_notable
        call    puts
        jmp     fd_fail
fdp_notfat:
        mov     edx,OFFSET msg_notfat
        call    puts
        jmp     fd_fail

;=====================================================================
; fd_getdp - the first argument, as <disk>:<partition>.
;
;   -> [pl_drive] = a BIOS drive number, [pl_idx] = a slot 0-3
;   CF set if it is not that shape.
;
; DISK AND SLOT TOGETHER, in one argument, because they are not
; independent: a slot number means nothing without the disk it is on,
; and two separate arguments can be given in the wrong order by
; somebody who is about to change a partition table.  "1:2" cannot be.
;=====================================================================
fd_getdp:
        push    eax
        push    ecx
        push    esi
        mov     esi,OFFSET fd_args
        call    fd_atoi                 ; the disk
        jc      fgd_bad
        cmp     eax,MAX_DISKS
        jae     fgd_bad
        add     eax,80h
        mov     [pl_drive],al
        cmp     byte ptr [esi],':'
        jne     fgd_bad
        inc     esi
        call    fd_atoi                 ; the slot, written 1-based as every
        jc      fgd_bad                 ; FDISK has always numbered them
        or      eax,eax
        jz      fgd_bad
        cmp     eax,PART_MAX
        ja      fgd_bad
        dec     eax
        mov     [pl_idx],eax
        cmp     byte ptr [esi],0
        jne     fgd_bad                 ; trailing rubbish is a typing mistake
        pop     esi                     ; and a typing mistake here is a
        pop     ecx                     ; partition table
        pop     eax
        clc
        ret
fgd_bad:
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret

;--- fd_atoi - ESI -> digits.  EAX = the number, ESI past it.
;--- CF set if there was not at least one digit, or it overflowed.
fd_atoi:
        push    ecx
        push    edx
        xor     eax,eax
        xor     ecx,ecx                 ; ECX = digits taken
fa_digit:
        movzx   edx,byte ptr [esi]
        cmp     dl,'0'
        jb      fa_end
        cmp     dl,'9'
        ja      fa_end
        sub     edx,'0'
        push    edx
        mov     edx,10
        mul     edx                     ; EDX:EAX - and EDX not being zero
        pop     edx                     ; afterwards is the overflow
        jc      fa_bad
        add     eax,edx
        jc      fa_bad
        inc     ecx
        inc     esi
        jmp     short fa_digit
fa_end:
        or      ecx,ecx
        jz      fa_bad
        pop     edx
        pop     ecx
        clc
        ret
fa_bad:
        pop     edx
        pop     ecx
        stc
        ret


;=====================================================================
; THE EXECUTOR.  RelocExecute(), ExecMove() and ExecResizeInPlace(),
; ported - and only those, because only those are the SAME-LAYOUT
; regime.
;
; WHAT SAME-LAYOUT MEANS, and why it is the whole safety argument:
; the cluster size and the FAT width are unchanged, so CLUSTER NUMBERS
; SURVIVE.  Cluster 5000 is still cluster 5000 when the volume has
; finished moving, which means every cluster chain in the FAT is still
; correct, every directory entry's starting cluster is still correct,
; and not one byte of file data or directory content has to be
; rewritten or even understood.  The engine moves three regions and
; lays down a new BPB, and that is all it does.
;
; The other regime - where the cluster size has to change - renumbers
; everything and would have to rebuild every chain and rewrite every
; directory entry recursively.  SAFEPART has that code and refuses to
; run it against a live volume because it has never been proven
; against disk images.  This port does not carry it at all: rl_plan
; answers RL_REMAP and says what it would take.  A refusal that is
; understood is worth more than a rebuild that is hoped for.
;
; NOTHING HERE RUNS WITHOUT /W.  dk_write refuses unarmed, so a run
; without it walks the whole planner, prints exactly what it would
; have done, and is physically incapable of changing a sector.
;=====================================================================
rl_exec:
        pushad
        cmp     byte ptr [dk_armed],0
        je      rx_notarmed
        cmp     dword ptr [pl_status],RL_OK
        jne     rx_badplan

        cmp     dword ptr [pl_op],OP_MOVE
        je      rx_move
        cmp     dword ptr [pl_op],OP_GROW
        je      rx_resize
        cmp     dword ptr [pl_op],OP_SHRINK
        je      rx_resize

        ; A move AND a resize at once.  Safe here only when the new
        ; range does not overlap the old one: the region relocate
        ; below moves three regions in an order chosen for an overlap
        ; that shares a start, and an arbitrary overlap can have a
        ; destination land on a source it has not read yet.  SAFEPART
        ; hands that case to a simulator; this port refuses it, and
        ; the user can do it in two steps - which is what the
        ; simulator would decide anyway on any volume it approved.
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]
        cmp     eax,[pl_ostart]
        jbe     rx_resize               ; entirely below the old range
        mov     eax,[pl_ostart]
        add     eax,[pl_olen]
        cmp     eax,[pl_nstart]
        jbe     rx_resize               ; entirely above it
        mov     dword ptr [pl_status],RL_REMAP
        jmp     rx_out

rx_move:
        call    rl_domove
        jc      rx_ioerr
        jmp     short rx_table
rx_resize:
        call    rl_doresize
        jc      rx_ioerr

;--- and last of all, the partition table ----------------------------
; LAST, and it matters that it is last.  Until this sector is written
; the table still describes the old volume, so a machine that loses
; power in the middle of the copy comes back describing a volume whose
; data has been half-moved - which is bad - but a table written FIRST
; would describe a volume whose data has not been moved at all, and
; the difference is that the second one is silently wrong and boots.
rx_table:
        mov     edx,OFFSET msg_wrtab
        call    puts
        imul    esi,dword ptr [pl_idx],PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        mov     eax,[pl_nstart]
        mov     [esi+PE_START],eax
        mov     eax,[pl_nlen]
        mov     [esi+PE_SECS],eax
        call    rl_typebyte              ; -> AL
        mov     [esi+PE_TYPE],al

        ; the CHS fields, rebuilt.  ONLY THIS ENTRY'S: SAFEPART
        ; repacks all four, which quietly "corrects" tables it was
        ; never asked about - and a partition editor that rewrites a
        ; neighbour's bytes as a side effect of touching this one has
        ; done something the user cannot see and did not ask for.
        mov     eax,[pl_nstart]
        lea     edi,[esi+PE_CHSBEG]
        call    lba_to_chs
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]
        dec     eax                     ; the LAST sector, not one past it
        lea     edi,[esi+PE_CHSEND]
        call    lba_to_chs

        mov     al,[pl_drive]
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_write
        jc      rx_ioerr
        mov     dword ptr [pl_status],RL_OK
        jmp     short rx_out

rx_notarmed:
        mov     dword ptr [pl_status],RL_NOTARMED
        jmp     short rx_out
rx_badplan:
        jmp     short rx_out
rx_ioerr:
        mov     dword ptr [pl_status],RL_WRITEERR
rx_out:
        popad
        ret

;=====================================================================
; rl_domove - the length is unchanged, so the volume is one block and
;             the only thing that changes inside it is the BPB's count
;             of hidden sectors.
;=====================================================================
rl_domove:
        pushad
        mov     edx,OFFSET msg_movevol
        call    puts
        mov     eax,[pl_ostart]
        mov     [rc_src],eax
        mov     eax,[pl_nstart]
        mov     [rc_dst],eax
        mov     eax,[pl_olen]
        mov     [rc_cnt],eax
        call    rl_copy
        jc      rm_bad
        call    rl_wrboot
        jc      rm_bad
        popad
        clc
        ret
rm_bad:
        popad
        stc
        ret

;=====================================================================
; rl_doresize - the block relocate.
;
; Three regions move: the FATs, the root directory, and the data.  The
; ORDER IS THE ENTIRE CORRECTNESS ARGUMENT, because the new positions
; overlap the old ones - the volume starts in the same place and only
; the FAT got longer or shorter, so everything after it slides.
;
;   GROWING: the data region moves RIGHT, into space that is being
;   freed behind it as it goes.  It must be copied BACK-TO-FRONT and
;   BEFORE the FATs are written, because the FATs grow rightward over
;   sectors the old data region still occupies.
;
;   SHRINKING: everything moves LEFT.  The FATs are written FIRST -
;   they now end to the left of where the old root began - and the
;   root and data follow, FRONT-TO-BACK.
;
; rl_copy picks front-to-back or back-to-front from the direction of
; the move, so each of those is one call.
;=====================================================================
rl_doresize:
        pushad
        ; where everything is now, and where it is going
        mov     eax,[pl_ostart]
        add     eax,[fv_rsvd]
        mov     [rr_ofat],eax
        mov     eax,[pl_nstart]
        add     eax,[fv_rsvd]
        mov     [rr_nfat],eax

        mov     eax,[fv_nfats]
        imul    eax,[fv_spf]
        add     eax,[rr_ofat]
        mov     [rr_oroot],eax
        mov     eax,[fv_nfats]
        imul    eax,dword ptr [fg_exec+FG_SPF]
        add     eax,[rr_nfat]
        mov     [rr_nroot],eax

        mov     eax,[rr_oroot]
        add     eax,[fv_rootsec]
        mov     [rr_odata],eax
        mov     eax,[rr_nroot]
        add     eax,[fv_rootsec]
        mov     [rr_ndata],eax

        mov     eax,[rr_ndata]
        cmp     eax,[rr_odata]
        jbe     rz_shrink

;--- growing: data and root first, rightwards ------------------------
        mov     edx,OFFSET msg_movedata
        call    puts
        mov     eax,[rr_odata]
        mov     [rc_src],eax
        mov     eax,[rr_ndata]
        mov     [rc_dst],eax
        mov     eax,[fv_usedsec]
        mov     [rc_cnt],eax
        call    rl_copy
        jc      rz_bad
        call    rz_root
        jc      rz_bad
        call    rz_fats
        jc      rz_bad
        jmp     short rz_boot

;--- shrinking: the FATs first, then root and data leftwards ---------
rz_shrink:
        call    rz_fats
        jc      rz_bad
        call    rz_root
        jc      rz_bad
        mov     edx,OFFSET msg_movedata
        call    puts
        mov     eax,[rr_odata]
        mov     [rc_src],eax
        mov     eax,[rr_ndata]
        mov     [rc_dst],eax
        mov     eax,[fv_usedsec]
        mov     [rc_cnt],eax
        call    rl_copy
        jc      rz_bad

rz_boot:
        call    rl_wrboot
        jc      rz_bad
        popad
        clc
        ret
rz_bad:
        popad
        stc
        ret

;--- the root directory, which never changes size --------------------
rz_root:
        push    eax
        mov     edx,OFFSET msg_moveroot
        call    puts
        mov     eax,[rr_oroot]
        mov     [rc_src],eax
        mov     eax,[rr_nroot]
        mov     [rc_dst],eax
        mov     eax,[fv_rootsec]
        mov     [rc_cnt],eax
        call    rl_copy
        pop     eax
        ret

;--- the FATs, which do -----------------------------------------------
; Every copy is made from the FIRST old FAT, which is the live one:
; the second is a mirror and copying a mirror of a mirror would carry
; a stale one forward if the two had ever disagreed.  A tail longer
; than the old FAT is zeroed, and zero in a FAT means free, which is
; exactly what the clusters a grow has just added are.
rz_fats:
        pushad
        mov     edx,OFFSET msg_movefat
        call    puts
        xor     ebp,ebp                 ; EBP = which FAT
rzf_one:
        cmp     ebp,[fv_nfats]
        jae     rzf_done
        mov     eax,ebp
        imul    eax,dword ptr [fg_exec+FG_SPF]
        add     eax,[rr_nfat]
        mov     [rzf_dst],eax
        xor     ebx,ebx                 ; EBX = the sector within the FAT
rzf_sec:
        cmp     ebx,dword ptr [fg_exec+FG_SPF]
        jae     rzf_next
        cmp     ebx,[fv_spf]
        jae     rzf_zero
        mov     al,[pl_drive]
        push    ebx
        add     ebx,[rr_ofat]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        pop     ebx
        jc      rzf_bad
        jmp     short rzf_put
rzf_zero:
        push    edi
        push    ecx
        mov     edi,[sec_buf]
        xor     eax,eax
        mov     ecx,SEC_BYTES / 4
        rep     stosd
        pop     ecx
        pop     edi
rzf_put:
        mov     al,[pl_drive]
        push    ebx
        add     ebx,[rzf_dst]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_write
        pop     ebx
        jc      rzf_bad
        inc     ebx
        jmp     short rzf_sec
rzf_next:
        inc     ebp
        jmp     short rzf_one
rzf_done:
        popad
        clc
        ret
rzf_bad:
        popad
        stc
        ret

;=====================================================================
; rl_copy - [rc_cnt] sectors from [rc_src] to [rc_dst] on [pl_drive].
;
; OVERLAP-SAFE BY DIRECTION and by nothing cleverer: a copy that moves
; data LEFT reads a sector its destination has not reached yet if it
; goes front to back, and one that moves RIGHT does the same if it
; goes back to front.  Choosing the direction from the sign of the
; move is the whole of it - and it is why a copy between regions that
; overlap in a way that is neither (a move AND resize at once) is
; refused rather than attempted.
;=====================================================================
RC_CHUNK        EQU     128             ; 64K, which is what cpy_buf is

rl_copy:
        pushad
        mov     eax,[rc_cnt]
        or      eax,eax
        jz      rc_done
        mov     eax,[rc_src]
        cmp     eax,[rc_dst]
        je      rc_done
        ja      rc_fwd                  ; moving LEFT: front to back

;--- moving right: back to front -------------------------------------
        mov     ebp,[rc_cnt]            ; EBP = sectors still to do
rc_back:
        or      ebp,ebp
        jz      rc_done
        mov     ecx,RC_CHUNK
        cmp     ecx,ebp
        jbe     rc_bhave
        mov     ecx,ebp
rc_bhave:
        sub     ebp,ecx                 ; EBP = the offset of this chunk
        mov     al,[pl_drive]
        mov     ebx,[rc_src]
        add     ebx,ebp
        mov     edi,[cpy_buf]
        push    ecx
        call    dk_read
        pop     ecx
        jc      rc_bad
        mov     al,[pl_drive]
        mov     ebx,[rc_dst]
        add     ebx,ebp
        mov     edi,[cpy_buf]
        push    ecx
        call    dk_write
        pop     ecx
        jc      rc_bad
        call    rc_dot
        jmp     short rc_back

;--- moving left: front to back --------------------------------------
rc_fwd:
        xor     ebp,ebp                 ; EBP = the offset reached
rc_fnext:
        mov     eax,[rc_cnt]
        cmp     ebp,eax
        jae     rc_done
        sub     eax,ebp
        mov     ecx,RC_CHUNK
        cmp     ecx,eax
        jbe     rc_fhave
        mov     ecx,eax
rc_fhave:
        mov     al,[pl_drive]
        mov     ebx,[rc_src]
        add     ebx,ebp
        mov     edi,[cpy_buf]
        push    ecx
        call    dk_read
        pop     ecx
        jc      rc_bad
        mov     al,[pl_drive]
        mov     ebx,[rc_dst]
        add     ebx,ebp
        mov     edi,[cpy_buf]
        push    ecx
        call    dk_write
        pop     ecx
        jc      rc_bad
        add     ebp,ecx
        call    rc_dot
        jmp     short rc_fnext

rc_done:
        popad
        clc
        ret
rc_bad:
        popad
        stc
        ret

;--- a dot per chunk.  A partition move is minutes of disk on real
;--- hardware and a screen that says nothing for minutes is a screen
;--- somebody resets.
rc_dot:
        push    eax
        mov     al,'.'
        call    putc
        pop     eax
        ret

;=====================================================================
; rl_wrboot - the new BPB, written at the volume's new start.
;
; It is the OLD boot sector with four fields changed, not a fresh one:
; whatever boot code was there stays there, the OEM name stays, the
; volume serial and label stay.  A resize is not a format and must not
; look like one afterwards.
;=====================================================================
rl_wrboot:
        pushad
        mov     edx,OFFSET msg_wrboot
        call    puts
        mov     al,[pl_drive]
        mov     ebx,[pl_ostart]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        jc      rw_bad
        mov     edi,[sec_buf]

        ; the total, in whichever of the two fields can hold it
        mov     eax,[pl_nlen]
        cmp     eax,10000h
        jae     rw_big
        mov     [edi+BPB_TOTSEC16],ax
        mov     dword ptr [edi+BPB_TOTSEC32],0
        jmp     short rw_rest
rw_big:
        mov     word ptr [edi+BPB_TOTSEC16],0
        mov     [edi+BPB_TOTSEC32],eax
rw_rest:
        mov     eax,dword ptr [fg_exec+FG_SPF]
        mov     [edi+BPB_SPF16],ax
        movzx   eax,word ptr [dsk_rec+DSK_SPT]
        mov     [edi+BPB_SECPERTRK],ax
        movzx   eax,word ptr [dsk_rec+DSK_HEADS]
        mov     [edi+BPB_NUMHEADS],ax

        ; hidden sectors: where the volume now begins, which is the
        ; one field a MOVE changes and the one thing inside the volume
        ; that knows where the volume is
        mov     eax,[pl_nstart]
        mov     [edi+BPB_HIDDSEC],eax

        mov     al,[pl_drive]
        mov     ebx,[pl_nstart]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_write
        jc      rw_bad
        popad
        clc
        ret
rw_bad:
        popad
        stc
        ret

;=====================================================================
; lba_to_chs - EAX = an LBA, EDI -> the three bytes of a partition
;              table CHS field.
;
; An LBA past what CHS can express is written as the FROZEN MAXIMUM -
; 1023/heads-1/spt - and not as a truncated cylinder number, because
; that is what every disk since the mid-nineties has in those fields
; and what every tool that reads them expects to find.  A truncated
; cylinder is a number that LOOKS legal and names the wrong place.
;=====================================================================
lba_to_chs:
        pushad
        movzx   ecx,word ptr [dsk_rec+DSK_SPT]
        movzx   ebx,word ptr [dsk_rec+DSK_HEADS]
        or      ecx,ecx
        jz      lc_out
        or      ebx,ebx
        jz      lc_out
        push    eax
        imul    ebx,ecx                 ; EBX = sectors per cylinder
        xor     edx,edx
        div     ebx                     ; EAX = cylinder, EDX = within it
        mov     ebp,eax                 ; EBP = the cylinder
        mov     eax,edx
        xor     edx,edx
        div     ecx                     ; EAX = head, EDX = sector - 1
        pop     ebx                     ; (the LBA back, unused from here)
        inc     edx                     ; sectors are numbered from one
        cmp     ebp,1023
        jbe     lc_pack
        mov     ebp,1023                ; the frozen maximum
        movzx   eax,word ptr [dsk_rec+DSK_HEADS]
        dec     eax
        movzx   edx,word ptr [dsk_rec+DSK_SPT]
lc_pack:
        mov     [edi],al                ; head
        mov     eax,ebp
        shr     eax,2
        and     al,0C0h                 ; the cylinder's top two bits...
        or      al,dl                   ; ...share a byte with the sector
        mov     [edi+1],al
        mov     eax,ebp
        mov     [edi+2],al              ; and the low eight
lc_out:
        popad
        ret

;=====================================================================
; rl_typebyte - the type byte a volume of this shape should carry.
;              -> AL
;
; The rule is not decorative.  04h means "FAT16 addressed by CHS and
; under 32MB", 06h means "FAT16 by CHS and over it", 0Eh means "FAT16
; by LBA" - and a BIOS or a DOS that meets a partition whose type byte
; promises CHS addressing for sectors CHS cannot reach will read the
; wrong place and be certain it is right.
;=====================================================================
rl_typebyte:
        push    ebx
        cmp     dword ptr [fg_exec+FG_BITS],12
        jne     rf_16
        mov     al,PT_FAT12
        pop     ebx
        ret
rf_16:
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]
        cmp     eax,[phy_chs]
        ja      rf_lba                  ; part of it is past CHS's reach
        mov     eax,[pl_nlen]
        cmp     eax,10000h              ; 65536 sectors is the 32MB line
        jae     rf_big
        mov     al,PT_FAT16S
        pop     ebx
        ret
rf_big:
        mov     al,PT_FAT16B
        pop     ebx
        ret
rf_lba:
        mov     al,PT_FAT16L
        pop     ebx
        ret


;=====================================================================
; /M and /S - move a partition, and resize one.
;
; BOTH GO THROUGH THE SAME FOUR STEPS and only the one line that sets
; up the proposal differs, which is deliberate: a move and a resize
; are the same operation with a different argument held constant, and
; two code paths that agree today would eventually stop agreeing.
;
;   1. work out what is being proposed
;   2. plan it, which writes nothing
;   3. print the plan, whether or not it is going to run
;   4. run it, but only with /W
;
; STEP THREE HAPPENS EVEN WHEN STEP FOUR WILL NOT.  A run without /W
; is not a refusal, it is a DRY RUN: it walks every check, prints
; exactly what would have happened, and cannot touch a sector because
; dk_write refuses unarmed.  That is the property SAFEPART's /WRITE
; flag was for and it is worth more here, where the program is being
; tested against real partition tables for the first time.
;=====================================================================
fd_change:
        cmp     dword ptr [fd_nargs],2
        jb      fdc_usage
        call    fd_getdp
        jc      fdc_usage

        movzx   edx,byte ptr [pl_drive]
        mov     edi,OFFSET dsk_rec
        mov     eax,0F10Ch
        int     21h
        jc      fdc_nodisk
        test    byte ptr [dsk_rec+DSK_FLAGS],DSKF_PRESENT
        jz      fdc_nodisk
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        mov     [phy_total],eax
        mov     eax,dword ptr [dsk_rec+DSK_CHS]
        mov     [phy_chs],eax

        mov     al,[pl_drive]
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_read
        jc      fdc_unread
        mov     edi,[mbr_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     fdc_notable

;--- the proposal ----------------------------------------------------
        imul    esi,dword ptr [pl_idx],PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        mov     eax,[esi+PE_START]
        mov     [pl_nstart],eax
        mov     eax,[esi+PE_SECS]
        mov     [pl_nlen],eax

        mov     esi,OFFSET fd_args + FD_ARGLEN
        call    fd_atosz
        jc      fdc_usage
        cmp     byte ptr [fd_cmd],FDC_MOVE
        jne     fdc_dosize
        mov     [pl_nstart],eax
        jmp     short fdc_doplan
fdc_dosize:
        mov     [pl_nlen],eax

;--- plan it ---------------------------------------------------------
fdc_doplan:
        call    rl_plan

        call    crlf
        mov     edx,OFFSET msg_prop
        call    puts
        mov     edx,OFFSET rl_opname
        movzx   eax,byte ptr [pl_op]
        imul    eax,10
        add     edx,eax
        call    puts
        mov     edx,OFFSET msg_from
        call    puts
        mov     eax,[pl_ostart]
        call    put1
        mov     edx,OFFSET msg_plus
        call    puts
        mov     eax,[pl_olen]
        call    put1
        mov     edx,OFFSET msg_arrow
        call    puts
        mov     eax,[pl_nstart]
        call    put1
        mov     edx,OFFSET msg_plus
        call    puts
        mov     eax,[pl_nlen]
        call    put1
        call    crlf

;--- what the planner said -------------------------------------------
        cmp     dword ptr [pl_status],RL_OK
        jne     fdc_refused

        cmp     byte ptr [pl_crossbl],0
        je      fdc_nowarn
        mov     edx,OFFSET msg_bootwarn
        call    puts
fdc_nowarn:
        mov     edx,OFFSET msg_willbe
        call    puts
        mov     edx,OFFSET msg_fat
        call    puts
        mov     eax,dword ptr [fg_exec+FG_BITS]
        call    put2d
        mov     edx,OFFSET msg_comma2
        call    puts
        mov     eax,dword ptr [fg_exec+FG_SPC]
        call    put1
        mov     edx,OFFSET msg_spclus2
        call    puts
        mov     eax,dword ptr [fg_exec+FG_CLUS]
        call    put1
        mov     edx,OFFSET msg_clus2
        call    puts

;--- and whether it is going to happen -------------------------------
        cmp     byte ptr [dk_armed],0
        jne     fdc_go
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done

fdc_go:
        mov     edx,OFFSET msg_doing
        call    puts
        call    rl_exec
        call    crlf
        cmp     dword ptr [pl_status],RL_OK
        jne     fdc_refused
        mov     edx,OFFSET msg_didit
        call    puts
        jmp     fd_done

;--- the one place a refusal is printed ------------------------------
; ONE place, and every path reaches it, so there is no way to add a
; check that refuses silently.
fdc_refused:
        mov     edx,OFFSET msg_refused
        call    puts
        mov     edx,OFFSET rl_stname
        mov     eax,[pl_status]
        cmp     eax,RL_FATTYPE
        jbe     fdc_known
        xor     eax,eax
fdc_known:
        imul    eax,RL_NAMELEN
        add     edx,eax
        call    puts
        call    crlf
        jmp     fd_fail

fdc_usage:
        jmp     fd_usage
fdc_nodisk:
        mov     edx,OFFSET msg_nodisk
        call    puts
        jmp     fd_fail
fdc_unread:
        mov     edx,OFFSET msg_unread
        call    puts
        jmp     fd_fail
fdc_notable:
        mov     edx,OFFSET msg_notable
        call    puts
        jmp     fd_fail

;--- fd_atosz - a size or a position: sectors, or megabytes with M --
;--- ESI -> the text.  EAX = sectors.  CF set if it is not a number.
;---
;--- SECTORS ARE THE DEFAULT and megabytes are the suffix, which is
;--- the opposite way round from how a person thinks - and is right
;--- anyway.  A partition table is written in sectors, every number
;--- this program prints is in sectors, and a tool that silently
;--- reinterpreted a bare number as megabytes would turn "make it
;--- 67536" - which is a number the report just printed - into a
;--- request for sixty-six gigabytes.
fd_atosz:
        call    fd_atoi
        jc      fas_bad
        mov     dl,[esi]
        cmp     dl,'M'
        je      fas_meg
        cmp     dl,'m'
        je      fas_meg
        cmp     dl,0
        jne     fas_bad
        clc
        ret
fas_meg:
        inc     esi
        cmp     byte ptr [esi],0
        jne     fas_bad
        push    edx
        mov     edx,2048                ; sectors in a megabyte
        mul     edx
        pop     edx
        jc      fas_bad                 ; and a megabyte count that does not
        clc                             ; fit in sectors is not one
        ret
fas_bad:
        stc
        ret

;=====================================================================
; The critical-error handler: fail everything, silently.
;=====================================================================
fd_crit:
        mov     al,CERR_FAIL
        iretd

;=====================================================================
; typename - EAX = a partition type byte, printed padded to a column.
;
; A LONGER TABLE THAN DRIVES.EXE'S, and deliberately so: DRIVES lists
; the volumes the kernel gave letters to, so it only ever meets types
; it understands.  This program is shown the table as it really is,
; and a partition editor that prints "unknown" for the thing occupying
; half the disk is no use to the person deciding what to do about it.
;=====================================================================
typename:
        push    ebx
        mov     ebx,OFFSET tn_tbl
tn_look:
        cmp     byte ptr [ebx],0FFh     ; FFh ends it, NOT zero: zero is
        je      tn_unknown              ; a real entry here
        cmp     al,[ebx]
        je      tn_found
        add     ebx,11
        jmp     short tn_look
tn_found:
        lea     edx,[ebx+1]
        call    puts
        pop     ebx
        ret
tn_unknown:
        mov     edx,OFFSET msg_unknown
        call    puts
        pop     ebx
        ret

;--- type byte, nine characters, and the '$' that 21h/09h stops at ---
tn_tbl:
        db  PT_FAT12,       'FAT12    $'
        db  PT_FAT16S,      'FAT16    $'
        db  PT_EXTENDED,    'Extended $'
        db  PT_FAT16B,      'FAT16B   $'
        db  PT_FAT32,       'FAT32    $'
        db  PT_FAT32L,      'FAT32-LBA$'
        db  PT_FAT16L,      'FAT16-LBA$'
        db  PT_EXTENDEDL,   'Ext-LBA  $'
        db  PT_HID_FAT12,   'hid FAT12$'
        db  PT_HID_FAT16S,  'hid FAT16$'
        db  PT_HID_FAT16B,  'hid FT16B$'
        db  PT_HID_FAT32,   'hid FAT32$'
        db  PT_HID_FAT32L,  'hid F32L $'
        db  PT_HID_FAT16L,  'hid F16L $'
        db  07h,            'NTFS/HPFS$'
        db  63h,            'Unix     $'
        db  82h,            'Linux swp$'
        db  83h,            'Linux    $'
        db  85h,            'Linux ext$'
        db  8Eh,            'Linux LVM$'
        db  0A5h,           'FreeBSD  $'
        db  0FFh

;=====================================================================
; sec2mb - EAX sectors -> EAX megabytes, rounded to nearest.
;
; ROUNDED, not truncated, and it matters at the top of the report: a
; 504MB disk truncates to 503 and somebody spends an afternoon
; wondering which sector went missing.
;=====================================================================
sec2mb:
        push    edx
        push    ecx
        add     eax,1024                ; half of 2048 sectors
        mov     ecx,2048
        xor     edx,edx
        div     ecx
        pop     ecx
        pop     edx
        ret

;=====================================================================
; Output helpers.  All through 21h/02h and 21h/09h - this is an
; ordinary program and has no more access than any other.
;=====================================================================
puts:
        push    eax
        mov     eax,0900h
        int     21h
        pop     eax
        ret
putc:
        push    eax
        push    edx
        movzx   edx,al
        mov     eax,0200h
        int     21h
        pop     edx
        pop     eax
        ret
sp1:
        push    eax
        mov     al,' '
        call    putc
        pop     eax
        ret
sp2:
        call    sp1
        jmp     short sp1
crlf:
        push    eax
        mov     al,13
        call    putc
        mov     al,10
        call    putc
        pop     eax
        ret

fd_upper:
        cmp     al,'a'
        jb      fu_out
        cmp     al,'z'
        ja      fu_out
        sub     al,20h
fu_out:
        ret

;--- EAX in hex, two digits ------------------------------------------
put2:
        push    eax
        shr     eax,4
        and     al,0Fh
        call    puthex1
        pop     eax
        push    eax
        and     al,0Fh
        call    puthex1
        pop     eax
        ret
puthex1:
        add     al,'0'
        cmp     al,'9'
        jbe     ph_out
        add     al,7
ph_out:
        call    putc
        ret

;--- EAX in decimal, right aligned in a column -----------------------
put1:
        push    ebx
        mov     ebx,1
        jmp     short put_n
put2d:
        push    ebx
        mov     ebx,2
        jmp     short put_n
put3:
        push    ebx
        mov     ebx,3
        jmp     short put_n
put5:
        push    ebx
        mov     ebx,5
        jmp     short put_n
put6:
        push    ebx
        mov     ebx,6
        jmp     short put_n
put7:
        push    ebx
        mov     ebx,7
        jmp     short put_n
put8:
        push    ebx
        mov     ebx,8
        jmp     short put_n
put10:
        push    ebx
        mov     ebx,10
        jmp     short put_n
put12:
        push    ebx
        mov     ebx,12
put_n:
        push    eax
        push    ecx
        push    edx
        push    edi
        mov     edi,OFFSET num_buf + 19
        mov     byte ptr [edi],'$'
        xor     ecx,ecx
pn_div:
        xor     edx,edx
        push    ebx
        mov     ebx,10
        div     ebx
        pop     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        inc     ecx
        or      eax,eax
        jnz     pn_div
pn_pad:
        cmp     ecx,ebx
        jae     pn_out
        dec     edi
        mov     byte ptr [edi],' '
        inc     ecx
        jmp     short pn_pad
pn_out:
        mov     edx,edi
        call    puts
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        pop     ebx
        ret

;=====================================================================
; Data
;=====================================================================
fd_cmd      db  0
fd_mbrok    db  0
dk_armed    db  0
dk_drive    db  0
fd_sw       dd  0
fd_nargs    dd  0
fd_ndisks   dd  0
fd_extbase  dd  0
fd_extcur   dd  0
vol_n       dd  0

mbr_buf     dd  0
ebr_buf     dd  0
sec_buf     dd  0
cpy_buf     dd  0

dsk_rec     db  DSK_LEN dup(0)

;--- the volume being planned against, as its BPB describes it -------
fv_valid    db  0
fv_drive    db  0
fv_bits     dd  0
fv_bps      dd  0
fv_spc      dd  0
fv_rsvd     dd  0
fv_nfats    dd  0
fv_rootent  dd  0
fv_spf      dd  0
fv_total    dd  0
fv_rootsec  dd  0
fv_datasec  dd  0
fv_clusters dd  0
fv_usedhigh dd  0
fv_usedsec  dd  0
fv_fatlba   dd  0

;--- the cluster-size chooser's working set --------------------------
fgw_total    dd  0
fgw_spc      dd  0
fgw_spf      dd  0
fgw_rootsec  dd  0
fgw_bits     dd  0
fgw_data     dd  0
fgw_clus     dd  0
fgw_want     dd  0
fgw_iter     dd  0

fg_ideal    db  FG_LEN dup(0)           ; smallest cluster: reported
fg_keep     db  FG_LEN dup(0)           ; the current cluster: executed
fg_exec     db  FG_LEN dup(0)           ; whichever was chosen

;--- the plan in flight ----------------------------------------------
pl_drive    db  0
pl_same     db  0
pl_boot     db  0
pl_crossbl  db  0
pl_keepok   db  0
pl_idx      dd  0
pl_ostart   dd  0
pl_olen     dd  0
pl_nstart   dd  0
pl_nlen     dd  0
pl_op       dd  0
pl_status   dd  0
pl_room     dd  0
rl_needcc   dd  0
rl_maxcc    dd  0
phy_total   dd  0
phy_chs     dd  0

;--- the region relocate's working set -------------------------------
rc_src      dd  0
rc_dst      dd  0
rc_cnt      dd  0
rr_ofat     dd  0
rr_nfat     dd  0
rr_oroot    dd  0
rr_nroot    dd  0
rr_odata    dd  0
rr_ndata    dd  0
rzf_dst     dd  0
vol_tbl     db  LASTDRIVE * DRV_LEN dup(0)
fd_args     db  FD_MAXARGS * FD_ARGLEN dup(0)
num_buf     db  20 dup(0)

msg_usage   db  13,10,'FDISK - the PM-DOS partition editor',13,10,13,10
            db  '  FDISK       report every disk',27h,'s partition table',13,10
            db  '  FDISK /L    ...and the logical drives inside the '
            db  'extended container',13,10
            db  '  FDISK /P d:p    what one partition is, and how far '
            db  'it can move',13,10
            db  '  FDISK /M d:p n  move it so that it starts at sector n',13,10
            db  '  FDISK /S d:p n  resize it to n sectors - or n MB, '
            db  'written 40M',13,10
            db  '  ...add /W       and it happens.  Without /W every '
            db  'one of those is a',13,10
            db  '                  dry run that says what it would '
            db  'have done.',13,10
            db  13,10
            db  '  d:p names a DISK and a PARTITION - 0:1 is the first '
            db  'slot of the',13,10
            db  '  first disk.  Slots are numbered 1 to 4 as every FDISK '
            db  'has numbered',13,10
            db  '  them; disks are numbered as FDISK reports them.',13,10
            db  13,10,'$'

msg_disk    db  'Disk $'
msg_sp2     db  '   $'
msg_cyls    db  ' cylinders, $'
msg_heads   db  ' heads, $'
msg_spt     db  ' sectors,$'
msg_mb      db  ' MB',13,10,'$'
msg_mb2     db  ' MB  $'
msg_reach   db  '         CHS reaches $'
msg_ofcap   db  ' of $'
msg_secs    db  ' sectors$'
msg_beyond  db  '  - the rest is out of a partition table',27h,'s CHS reach$'

msg_thead   db  13,10,' #  A  Type            Start       Sectors'
            db  '     Size  Vol',13,10
            db  ' -  -  ---------  ----------  ------------'
            db  '  -------  ---',13,10,'$'
msg_empty   db  '     (empty)',13,10,'$'
msg_notable db  '         no partition table (no AA55 signature)',13,10,'$'
msg_unread  db  '         LBA 0 will not read',13,10,'$'
msg_lmark   db  '     L  $'
msg_badebr  db  '         the extended chain stops at an unreadable '
            db  'record',13,10,'$'
msg_chloop  db  '         the extended chain points at itself',13,10,'$'
msg_chlong  db  '         the extended chain is longer than this walk '
            db  'follows',13,10,'$'
msg_unknown db  'unknown  $'
msg_part    db  ' partition $'
msg_sp3     db  '   $'
msg_at      db  '  at $'
msg_comma   db  ', $'
msg_secmb   db  ' sectors ($'
msg_mbclose db  ' MB)$'
msg_vol     db  13,10,'  $'
msg_fat     db  'FAT$'
msg_spclus  db  ' sectors/cluster   $'
msg_fats    db  ' FATs   $'
msg_rootent db  ' root entries   $'
msg_spfat   db  ' sectors/FAT$'
msg_clusters db ' clusters   $'
msg_datasec db  ' data sectors$'
msg_inuse   db  13,10,13,10,'  in use            $'
msg_empty2  db  'nothing - the volume is empty',13,10,'$'
msg_upto    db  ' is the highest cluster,$'
msg_room    db  13,10,13,10,'  room at this start  $'
msg_shrink  db  13,10,'  can shrink to      $'
msg_grow    db  13,10,'  can grow to        $'
msg_nolimit db  'not at all - see what is in use above$'
msg_ideal   db  13,10,13,10,'  the smallest cluster this size could take is $'
msg_idealto db  ' sector(s), making it $'
msg_idealend db ' - which this program reports and does',13,10
            db  '  not do: changing the cluster size renumbers every '
            db  'cluster',13,10,'$'
msg_nodisk  db  'No such disk.',13,10,'$'
msg_notfat  db  'That partition is not a FAT12 or FAT16 volume this can '
            db  'resize.',13,10,'$'

;--- what one operation is called, ten bytes a row -------------------
rl_opname:
        db  'nothing  $'                ; OP_NONE
        db  'a move   $'                ; OP_MOVE
        db  'a grow   $'                ; OP_GROW
        db  'a shrink $'                ; OP_SHRINK
        db  'move+size$'                ; OP_MOVERS

;--- and why one was refused, RL_NAMELEN bytes a row -----------------
; EVERY ROW SAYS WHAT TO DO NEXT, which is the difference between an
; error message and a diagnosis.  "Cannot shrink" tells somebody they
; are stuck; "something is stored above the new end, and /P says how
; far down it goes" tells them to defragment and try again.
rl_stname:
        db  'it can be done.                                        $'
        db  'the new range is the range it already has.             $'
        db  'that is not a FAT12 or FAT16 volume.                   $'
        db  'a sector would not read.                               $'
        db  'a sector would not write - the disk may be damaged.    $'
        db  'something is stored above the new end.  /P says where. $'
        db  'it would run into the partition next to it.            $'
        db  'it would run off the disk, or over the boot record.    $'
        db  'no legal FAT layout exists at that size.               $'
        db  'writes are not armed.  Add /W to do it for real.       $'
        db  'the cluster size would have to change, which renumbers $'
        db  'FAT12 and FAT16 would change places, which renumbers   $'

msg_prop    db  '  proposed: $'
msg_from    db  '  -  from $'
msg_plus    db  ' + $'
msg_arrow   db  '  to  $'
msg_willbe  db  '  it would then be $'
msg_comma2  db  ', $'
msg_spclus2 db  ' sectors/cluster, $'
msg_clus2   db  ' clusters',13,10,'$'
msg_bootwarn db 13,10,'  NOTE: it is the ACTIVE partition and part of it '
            db  'would sit past',13,10,'  the 1024-cylinder line, where a '
            db  'BIOS with only CHS cannot',13,10,'  boot from it.',13,10,'$'
msg_dryrun  db  13,10,'  Nothing was written.  This was a dry run: add /W '
            db  'to do it.',13,10,'$'
msg_doing   db  13,10,'  Doing it$'
msg_movevol db  13,10,'  moving the volume$'
msg_movedata db 13,10,'  moving the data$'
msg_moveroot db 13,10,'  moving the root directory$'
msg_movefat db  13,10,'  writing the file allocation tables$'
msg_wrboot  db  13,10,'  writing the boot record$'
msg_wrtab   db  13,10,'  writing the partition table$'
msg_didit   db  13,10,'  Done.  REBOOT before using the disk: every drive '
            db  'letter on it was',13,10,'  worked out at boot from the table '
            db  'this has just changed.',13,10,'$'
msg_refused db  13,10,'  Refused: $'

img_end:                                ; the BSS begins here

_FDK    ENDS
        END
