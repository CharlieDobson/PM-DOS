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

MBRW_LEN        EQU     1B8h            ; the boot code of a master boot
                                        ; record, and nothing past it: 1B8h
                                        ; is where the disk identity lives
MAX_DISKS       EQU     8               ; the ceiling on what F1h/0Ch may
                                        ; report.  It answers with the real
                                        ; number; this only bounds the loop

SEC_BYTES       EQU     512
FDC_REPORT      EQU     0
FDC_PLAN        EQU     1
FDC_MOVE        EQU     2
FDC_SIZE        EQU     3
FDC_CREATE      EQU     4
FDC_DELETE      EQU     5
FDC_ACTIVE      EQU     6
FDC_MBR         EQU     7
FDC_MENU        EQU     8
FDC_FORMAT      EQU     9               ; the screen's only: see fd_format

;--- what dk_write does with what it is given ------------------------
DK_STAGE        EQU     2               ; dk_armed: 0 writes nothing (the
                                        ; screen's dry run), 1 writes, and
                                        ; this keeps the sectors for the
                                        ; screen's Commit (FDPEND.INC)

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
FAT32_MAXCLUS   EQU     268435444       ; 0FFFFFF4h - and above that there
                                        ; is nothing, which is the one of
                                        ; these three bounds that is a
                                        ; limit of the FORMAT and not of
                                        ; what happens to fit
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
RL_NOTARMED     EQU     9               ; writes held off: the screen's dry run
RL_REMAP        EQU     10              ; no cluster size this program makes
                                        ; fits the new size
RL_FATTYPE      EQU     11              ; it would take FAT16 <-> FAT32
RL_HOLDS        EQU     12              ; a container: its chain runs past
                                        ; the new end
RL_CHAIN        EQU     13              ; a container whose chain does not
                                        ; read cleanly
RL_BADCLUS      EQU     14              ; the clusters would have to grow, and
                                        ; some are marked bad
RL_DAMAGED      EQU     15              ; a chain or a directory that does
                                        ; not hold together
RL_NOMEM        EQU     16              ; not enough memory for the tables
RL_LAST         EQU     RL_NOMEM        ; the last row of rl_stname

;--- what a resize does to the clusters (FDREMAP.INC) ----------------
RM_NONE         EQU     0               ; nothing: the same layout
RM_WIDTH        EQU     1               ; FAT12 <-> FAT16, the same clusters
RM_SPLIT        EQU     2               ; smaller clusters: nothing moves
RM_MERGE        EQU     3               ; larger clusters: files are packed
RM_SPCMAX       EQU     64              ; the largest cluster made: 32K
RM_EOC          EQU     0FFFFFFFFh      ; a FAT entry once rm_decode has read
RM_BADCL        EQU     0FFFFFFFEh      ; it: 0 free, these two, or the next
                                        ; cluster, whatever the width was

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

;--- one link of the extended chain, as fd_chain reads it ------------
; THE TABLE IS ABSOLUTE and the disk is relative, which is the whole
; reason it exists.  Every number in an extended boot record is an
; offset from something - and from two different somethings - so a
; program that keeps working in those terms spends its life deciding
; which base a given field wants.  Read once, converted once, and
; every later decision is arithmetic on plain sector numbers.
CH_EBR          EQU     00h             ; dd - where this record is
CH_VSTART       EQU     04h             ; dd - its volume, 0 if it has none
CH_VSECS        EQU     08h             ; dd
CH_EXTENT       EQU     0Ch             ; dd - record, gap and volume
                                        ;      together: what the link COSTS
CH_NEXT         EQU     10h             ; dd - the next record, 0 at the end
CH_TYPE         EQU     14h             ; db - the volume's type byte
CH_LEN          EQU     18h

CHN_OK          EQU     0
CHN_BADEBR      EQU     1               ; a record that would not read, or
                                        ; had no signature
CHN_LOOP        EQU     2               ; a next pointer that goes nowhere new
CHN_LONG        EQU     3               ; more links than EBR_MAX
CHN_RANGE       EQU     4               ; a next pointer outside the container

;--- one wall the partition being planned must not run into ----------
NB_START        EQU     00h             ; dd
NB_SECS         EQU     04h             ; dd
NB_LEN          EQU     08h
NB_MAX          EQU     EBR_MAX + 4     ; every link of a chain, or every
                                        ; other primary slot, and the one
                                        ; extra a logical drive always has:
                                        ; its own boot record

; TWO SEGMENTS, ONE GROUP, as PMDOS.SYS is laid out: _FDK is the file
; and _FDB is the BSS behind it, which the link places after the code
; and the raw output does not write.  The screen's sixteen kilobytes
; of buffers used to be zeros in the file, and the floppy is full.
FDG     GROUP   _FDK, _FDB

_FDB    SEGMENT USE32 DWORD PUBLIC 'BSS'
bss_start   LABEL BYTE
_FDB    ENDS

_FDK    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:FDG, DS:FDG, ES:FDG, SS:FDG
        ORG     0

;--- what the BSS holds; the equates that size it are all above -----
FD_BSS_LEN      EQU     SEC_BYTES*4 + 65536 + CH_LEN*EBR_MAX
                                        ; four sectors, the 64K the relocator
                                        ; copies through, and the chain table
;--- the changes the screen holds until Commit (FDPEND.INC) ----------
OV_MAX          EQU     256             ; sectors staged: a table, a chain's
                                        ; records and its volumes' boot
                                        ; sectors.  Never data
JN_MAX          EQU     32              ; changes waiting
JN_LEN          EQU     0D0h            ; one of them; FDPEND.INC lays it
                                        ; out and checks it fits
PD_BSS_LEN      EQU     OV_MAX*SEC_BYTES + OV_MAX*4 + OV_MAX + JN_MAX*JN_LEN
TUI_BSS_LEN     EQU     82 + 140*40 + 512*4 + 8192 + 4000 + PD_BSS_LEN
                                        ; the screen's buffers, as the end of
                                        ; this file lays them out - and the
                                        ; end of this file checks the sum

;=====================================================================
; D32 header.  The buffers live in the BSS: four sectors and the
; 64K block the relocator will copy through.
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET fd_entry>,<FD_BSS_LEN + TUI_BSS_LEN + 16>,4096
                                        ; +16: the BSS segment is aligned

;=====================================================================
fd_entry:
        mov     eax,OFFSET bss_start    ; carve up the BSS
        mov     [mbr_buf],eax
        add     eax,SEC_BYTES
        mov     [ebr_buf],eax
        add     eax,SEC_BYTES
        mov     [sec_buf],eax
        add     eax,SEC_BYTES
        mov     [cpy_buf],eax
        add     eax,65536
        mov     [eb2_buf],eax           ; the SECOND extended record the
        add     eax,SEC_BYTES           ; chain surgery has open at once
        mov     [chn_tbl],eax

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
        cmp     byte ptr [fd_cmd],FDC_CREATE
        je      fd_create
        cmp     byte ptr [fd_cmd],FDC_DELETE
        je      fd_delete
        cmp     byte ptr [fd_cmd],FDC_ACTIVE
        je      fd_active
        cmp     byte ptr [fd_cmd],FDC_MBR
        je      fd_mbr
        cmp     byte ptr [fd_cmd],FDC_MENU
        je      fd_tui
        jmp     fd_usage

;---------------------------------------------------------------------
fd_help:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     eax,4C00h
        int     21h

fd_usage:
        ; THE USAGE SCREEN IS AN ANSWER TO A COMMAND LINE.  Inside the
        ; menu the argument came off a prompt that said what it wanted,
        ; so twenty lines about switches would be answering a question
        ; nobody asked - one sentence and back to the menu is the same
        ; refusal at the right volume.
        cmp     byte ptr [fd_menu],0
        jne     fu_menu
        mov     edx,OFFSET msg_usage
        call    puts
        mov     eax,4C01h
        int     21h
fu_menu:
        mov     edx,OFFSET msg_mbadarg
        call    puts
        jmp     short fd_fail

fd_done:
        mov     byte ptr [fd_rc],0
        cmp     byte ptr [fd_menu],0
        jne     fd_back
        mov     eax,4C00h
        int     21h

;--- an error that has already said what it was ----------------------
fd_fail:
        mov     byte ptr [fd_rc],1
        cmp     byte ptr [fd_menu],0
        jne     fd_back
        mov     eax,4C01h
        int     21h

;--- and back to the screen, from wherever the verb happened to be ---
; THE STACK IS DISCARDED, NOT UNWOUND.  Every verb in this program
; ends by jumping to fd_done or fd_fail, and it can do that from four
; calls deep - so what sits above tui_run's frame is saved registers
; belonging to frames that will never return.  There is nothing in
; any of them that has to be put back: this program owns no handles,
; holds no locks, and allocates nothing.  So the honest thing is to
; say so and set ESP where tui_run left it, which is a longjmp written
; out in the two instructions it actually is - and tui_after is a RET,
; so the verb "returns" to whoever called tui_run.
fd_back:
        mov     esp,[fd_mstack]
        jmp     tui_after

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
        ; NOTHING AFTER THE NAME IS THE SCREEN, as it has been for every
        ; FDISK since 5.0; the report those used to print is /STATUS,
        ; which is what MS-DOS calls it, and /L still prints it with
        ; the chain walked.
        mov     byte ptr [fd_cmd],FDC_MENU
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
        jmp     fp_arg
fp_skip:
        inc     esi
        dec     ecx
        jmp     short fp_next

;--- a switch: one letter, folded up ---------------------------------
fp_switch:
        inc     esi
        dec     ecx
        jz      fp_swbad
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
        cmp     al,'C'
        je      fp_sw_c
        cmp     al,'D'
        je      fp_sw_d
        cmp     al,'A'
        je      fp_sw_a
        cmp     al,'I'
        je      fp_sw_i
        ; /? AND /H PRINT THE SAME THING AND ARE NOT ERRORS.  They
        ; used to fall into fp_bad, which prints the usage as well -
        ; so the screen was right and the exit code was 1, and a
        ; batch file that asked FDISK for help was told it had made a
        ; mistake.
        cmp     al,'?'
        je      fp_swhelp
        cmp     al,'H'
        je      fp_swhelp
        jmp     fp_bad
; fp_swbad is a trampoline, like fp_argbad: this list of switch
; letters outgrew the reach of a short conditional jump.
fp_swbad:
        jmp     fp_bad
fp_swhelp:
        jmp     fd_help
fp_sw_l:
        or      dword ptr [fd_sw],FDS_LOGICAL
        cmp     byte ptr [fd_cmd],FDC_MENU
        jne     fp_sw_lnext             ; /L on its own is the report
        mov     byte ptr [fd_cmd],FDC_REPORT
fp_sw_lnext:
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_p:
        mov     byte ptr [fd_cmd],FDC_PLAN
        inc     esi
        dec     ecx
        jmp     fp_next
; /M IS A MOVE AND /MBR IS THE BOOT CODE, and they are told apart
; here rather than given two unrelated letters, because /MBR is what
; every FDISK since 5.0 has called it and a program that spelled it
; differently would be the only one that did.  It is the only switch
; in this parser longer than a letter, which is why the test is
; written out in full instead of going through a table: one exception
; is an exception, and a table would be pretending there are more.
fp_sw_m:
        mov     byte ptr [fd_cmd],FDC_MOVE
        inc     esi
        dec     ecx
        cmp     ecx,2
        jb      fp_next                 ; too short to be anything else
        mov     al,[esi]
        call    fd_upper
        cmp     al,'B'
        jne     fp_next
        mov     al,[esi+1]
        call    fd_upper
        cmp     al,'R'
        jne     fp_next
        mov     byte ptr [fd_cmd],FDC_MBR
        add     esi,2
        sub     ecx,2
        jmp     fp_next
; /S IS A RESIZE AND /STATUS IS THE REPORT, told apart the way /M and
; /MBR are: the one other switch longer than a letter, and the one
; MS-DOS's own FDISK takes, so a batch file written for that one
; gets the same answer here.
fp_sw_s:
        mov     byte ptr [fd_cmd],FDC_SIZE
        inc     esi
        dec     ecx
        cmp     ecx,5
        jb      fp_next
        push    esi
        push    ecx
        mov     edi,OFFSET msg_status
fp_sw_st:
        mov     al,[esi]
        call    fd_upper
        cmp     al,[edi]
        jne     fp_sw_snot
        inc     esi
        inc     edi
        cmp     byte ptr [edi],0
        jne     fp_sw_st
        pop     ecx
        pop     esi
        mov     byte ptr [fd_cmd],FDC_REPORT
        add     esi,5
        sub     ecx,5
        jmp     fp_next
fp_sw_snot:
        pop     ecx
        pop     esi
        jmp     fp_next
fp_sw_c:
        mov     byte ptr [fd_cmd],FDC_CREATE
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_d:
        mov     byte ptr [fd_cmd],FDC_DELETE
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_a:
        mov     byte ptr [fd_cmd],FDC_ACTIVE
        inc     esi
        dec     ecx
        jmp     fp_next
fp_sw_i:
        mov     byte ptr [fd_cmd],FDC_MENU
        inc     esi
        dec     ecx
        jmp     fp_next

;--- an argument: text up to the next separator ----------------------
; fp_argbad is a trampoline because the switch list above grew past
; the reach of a short conditional jump when /C, /D and /A went in.
fp_argbad:
        jmp     fp_bad
fp_arg:
        mov     eax,[fd_nargs]
        cmp     eax,FD_MAXARGS
        jae     fp_argbad
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
        jae     fp_argbad
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

        mov     byte ptr [fd_misal],0
        call    fr_head
        call    fr_table
        test    dword ptr [fd_sw],FDS_LOGICAL
        jz      fr_alnote
        call    fr_logicals
fr_alnote:
        cmp     byte ptr [fd_misal],0
        je      fr_nextdisk
        mov     edx,OFFSET msg_alnote
        call    puts
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
        mov     edx,OFFSET msg_mb3
        call    puts

        ; DOES IT BEGIN AND END WHERE A PARTITION SHOULD?  This costs
        ; one column and answers a question that otherwise takes a
        ; calculator: a table written by another tool - or by hand, or
        ; by a version of this one from before /C aligned anything -
        ; can be perfectly legal and still sit across cylinder
        ; boundaries, which is what makes a later create or resize
        ; come back with numbers nobody expected.  Reporting it is not
        ; a complaint.  Nothing here will correct it: moving somebody
        ; else's partition to tidy the arithmetic is exactly the kind
        ; of unasked-for write this program does not do.
        push    ebx
        mov     eax,[esi+PE_START]
        mov     edx,eax
        add     edx,[esi+PE_SECS]
        movzx   ebx,word ptr [dsk_rec+DSK_SPT]
        call    fd_aligned
        mov     al,' '
        jnc     ft_algok
        mov     al,'*'
        mov     byte ptr [fd_misal],1
ft_algok:
        call    putc
        pop     ebx

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
; fr_logicals - the extended chain of the disk in EBX, reported.
;
; The walk itself is fd_chain's; this only prints what it found.  The
; NUMBERING is the part worth arguing about: a logical drive is named
; 5, 6, 7..., carrying on past the four primary slots, because that is
; how the report is read and then typed back as /D 1:6 - and because
; a logical drive genuinely has no slot of its own to be numbered by,
; so any scheme here is a convention, and this is the convention every
; other tool of the last thirty years already uses.
;---------------------------------------------------------------------
fr_logicals:
        pushad
        cmp     byte ptr [fd_mbrok],0
        je      fl_out
        lea     eax,[ebx+80h]
        mov     [pl_drive],al           ; which is how fd_chain names a disk
        call    fd_chain
        jc      fl_out                  ; no container: nothing to report

        xor     ebp,ebp
fl_row:
        cmp     ebp,[chn_n]
        jae     fl_stat
        imul    esi,ebp,CH_LEN
        add     esi,[chn_tbl]
        cmp     dword ptr [esi+CH_VSTART],0
        je      fl_rowstep              ; a link carrying no volume, which is
        call    fl_showvol              ; legal and has nothing to name
fl_rowstep:
        inc     ebp
        jmp     short fl_row

;--- and why the walk stopped, when it was not the end of the chain --
fl_stat:
        mov     eax,[chn_st]
        cmp     eax,CHN_OK
        je      fl_out
        cmp     eax,CHN_BADEBR
        je      fl_sbad
        cmp     eax,CHN_LOOP
        je      fl_sloop
        cmp     eax,CHN_RANGE
        je      fl_srange
        mov     edx,OFFSET msg_chlong
        jmp     short fl_say
fl_sbad:
        mov     edx,OFFSET msg_badebr
        jmp     short fl_say
fl_sloop:
        mov     edx,OFFSET msg_chloop
        jmp     short fl_say
fl_srange:
        mov     edx,OFFSET msg_chrange
fl_say:
        call    puts
fl_out:
        popad
        ret

;--- one logical drive.  ESI -> its row of chn_tbl, EBP = its index --
fl_showvol:
        pushad
        lea     eax,[ebp+5]
        call    put2d
        call    sp1
        mov     al,' '                  ; no logical drive is ever the active
        call    putc                    ; one - the MBR is where booting looks
        call    sp2
        movzx   eax,byte ptr [esi+CH_TYPE]
        call    put2
        call    sp1
        movzx   eax,byte ptr [esi+CH_TYPE]
        call    typename

        mov     eax,[esi+CH_VSTART]
        call    put10
        mov     eax,[esi+CH_VSECS]
        call    put12
        mov     eax,[esi+CH_VSECS]
        call    sec2mb
        call    put7
        mov     edx,OFFSET msg_mb3
        call    puts

        ; THE EXTENT MEASURED FOR ALIGNMENT IS THE LINK'S, not the
        ; volume's.  A logical drive's own boot record sits in front of
        ; it and belongs to it, so it is the RECORD that lands on a
        ; cylinder boundary and the volume that starts one track in -
        ; which is exactly the master boot record's relationship to the
        ; first primary partition, one level down.
        push    ebx
        mov     eax,[esi+CH_EBR]
        mov     edx,eax
        add     edx,[esi+CH_EXTENT]
        mov     ebx,[chn_base]          ; the head of a chain has to be where
        call    fd_aligned              ; the container starts, boundary or not
        mov     al,' '
        jnc     fl_algok
        mov     al,'*'
        mov     byte ptr [fd_misal],1
fl_algok:
        call    putc
        pop     ebx

        mov     eax,[esi+CH_VSTART]
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
; fd_chain - THE EXTENDED CHAIN, READ ONCE.
;
; The disk named in pl_drive, whose table is already in mbr_buf.  One
; absolute row of chn_tbl per link, and:
;
;   chn_slot  which primary slot holds the container
;   chn_base  the container's first sector - the base every slot-1
;             offset in the whole chain is measured from
;   chn_ext   how many sectors the container is
;   chn_n     how many links were followed
;   chn_st    CHN_OK, or why the walk stopped short of the end
;
; CF set when the disk has no extended container at all, which is not
; an error - it is most disks.
;
; THE TWO "START" FIELDS IN AN EXTENDED BOOT RECORD ARE RELATIVE TO
; DIFFERENT THINGS and that is the whole difficulty of the format:
; slot 0's start is relative to the record that holds it, and slot
; 1's - the pointer to the next record - is relative to the FIRST
; record, the one the master boot record named.  Getting that wrong
; produces a chain that walks a little further each time and ends up
; reading a sector that is nobody's.
;
; THE WALK NOW INSISTS THE CHAIN RUNS FORWARD, which counting links
; alone did not.  Every tool that has ever written one of these wrote
; it in ascending order, because the list is laid out in the space it
; describes; a pointer that goes backwards, or outside the container,
; is a table something has damaged.  Refusing to walk it is the whole
; point of the change - this program EDITS the chain now, and surgery
; on a structure whose shape could not be established is how a disk
; with one broken link comes back with all of them broken.
;=====================================================================
fd_chain:
        pushad
        mov     dword ptr [chn_n],0
        mov     dword ptr [chn_st],CHN_OK
        mov     dword ptr [chn_slot],0FFFFFFFFh

        ; THE FIRST CONTAINER WINS, because DOS only ever followed one
        ; chain per disk and a table with two is a table something else
        ; made - and the one thing worse than not editing the second is
        ; editing it as though the first were not there.
        mov     esi,[mbr_buf]
        add     esi,PART_OFF
        xor     ebp,ebp
fc_find:
        mov     al,[esi+PE_TYPE]
        call    fd_isext
        jnc     fc_got
        add     esi,PE_LEN
        inc     ebp
        cmp     ebp,PART_MAX
        jb      fc_find
        popad
        stc
        ret
fc_got:
        mov     [chn_slot],ebp
        mov     al,[esi+PE_TYPE]
        mov     [chn_ctype],al          ; 05h or 0Fh - a link this program
                                        ; adds is spelled the way the
                                        ; container it goes in is spelled
        mov     eax,[esi+PE_START]
        mov     [chn_base],eax
        mov     [chn_cur],eax
        mov     eax,[esi+PE_SECS]
        mov     [chn_ext],eax

        mov     edi,[chn_tbl]
fc_step:
        cmp     dword ptr [chn_n],EBR_MAX
        jae     fc_long

        mov     al,[pl_drive]
        mov     ebx,[chn_cur]
        mov     ecx,1
        push    edi
        mov     edi,[ebr_buf]
        call    dk_read
        pop     edi
        jc      fc_bad                  ; a record that will not read is a
                                        ; fault wherever in the chain it is
        mov     esi,[ebr_buf]
        cmp     word ptr [esi+1FEh],MBR_SIG
        jne     fc_nosig

        ; the link, converted once into plain sector numbers
        mov     eax,[chn_cur]
        mov     [edi+CH_EBR],eax
        mov     dword ptr [edi+CH_VSTART],0
        mov     dword ptr [edi+CH_VSECS],0
        mov     dword ptr [edi+CH_NEXT],0
        mov     byte ptr [edi+CH_TYPE],PT_EMPTY
        mov     dword ptr [edi+CH_EXTENT],1     ; an empty link still costs
                                                ; the sector it is written in
        add     esi,PART_OFF
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      fc_novol
        mov     al,[esi+PE_TYPE]
        mov     [edi+CH_TYPE],al
        mov     eax,[esi+PE_START]
        add     eax,[chn_cur]           ; slot 0 is relative to THIS record
        mov     [edi+CH_VSTART],eax
        mov     edx,[esi+PE_SECS]
        mov     [edi+CH_VSECS],edx
        add     eax,edx
        sub     eax,[chn_cur]
        mov     [edi+CH_EXTENT],eax     ; record, gap and volume together
fc_novol:
        inc     dword ptr [chn_n]

        ; slot 1 is relative to the CONTAINER, and is the only field in
        ; the whole structure that is
        mov     esi,[ebr_buf]
        add     esi,PART_OFF + PE_LEN
        mov     al,[esi+PE_TYPE]
        call    fd_isext
        jc      fc_done
        mov     eax,[esi+PE_START]
        or      eax,eax
        jz      fc_done
        add     eax,[chn_base]
        jc      fc_range
        cmp     eax,[chn_cur]
        jbe     fc_loop
        mov     edx,[chn_base]
        add     edx,[chn_ext]
        cmp     eax,edx
        jae     fc_range
        mov     [edi+CH_NEXT],eax
        mov     [chn_cur],eax
        add     edi,CH_LEN
        jmp     fc_step

fc_done:
        popad
        clc
        ret
fc_nosig:
        ; A HEAD RECORD WITH NO SIGNATURE IS AN EMPTY CONTAINER and not
        ; a fault.  It is exactly what this program leaves behind when
        ; the last logical drive of a chain is deleted, and what a
        ; container that has been created and not yet filled holds.
        cmp     dword ptr [chn_n],0
        je      fc_done
fc_bad:
        mov     dword ptr [chn_st],CHN_BADEBR
        jmp     short fc_done
fc_loop:
        mov     dword ptr [chn_st],CHN_LOOP
        jmp     short fc_done
fc_range:
        mov     dword ptr [chn_st],CHN_RANGE
        jmp     short fc_done
fc_long:
        mov     dword ptr [chn_st],CHN_LONG
        jmp     short fc_done

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
        ; AND WHAT THE SCREEN HAS STAGED, laid over what the disk said, so
        ; that every verb plans against the disk as it will be (FDPEND.INC)
        cmp     dword ptr [ov_n],0
        je      dr_clean
        call    ov_patch
dr_clean:
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
; SAFEPART's /WRITE flag, which this program had as /W, is gone from
; the command line: whoever types FDISK /M or /D means it, and the
; writes are armed from the start.  THE GATE STAYS FOR THE SCREEN,
; which runs every change once with dk_armed clear to find out what it
; would do (tui_dryrun) - and that run is physically unable to change
; a sector, whatever path the verb takes.
;
; dk_wrote is set BEFORE the first sector goes, not after the last: a
; move that fails half way has changed the disk as surely as one that
; finished, and the screen asks this to decide whether quitting has to
; restart the machine.
;
; STAGED (DK_STAGE), the sectors go into the screen's overlay and not
; to the disk - the screen holds every change until Commit, and the
; overlay is how the next change sees the last one.  Only a table and
; the records of a chain are ever written this way; a verb that tried
; to stage data would fill the overlay and be told it failed.
;---------------------------------------------------------------------
dk_write:
        cmp     byte ptr [dk_armed],0
        je      dw_notarmed
        cmp     byte ptr [dk_armed],DK_STAGE
        je      dw_stage
        mov     byte ptr [dk_wrote],1
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
dw_stage:
        pushad
        mov     esi,edi
dws_one:
        or      ecx,ecx
        jz      dws_ok
        call    ov_put
        jc      dws_full
        inc     ebx
        add     esi,SEC_BYTES
        dec     ecx
        jmp     short dws_one
dws_ok:
        popad
        clc
        ret
dws_full:
        popad
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
        test    byte ptr [esi+DRV_FLAGS],DRVF_CDROM OR DRVF_DRIVER OR DRVF_IMAGE
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
; what the executor is pointed at.  Only when the current cluster size
; cannot express the new size at all does it change, to the nearest
; one that can (FDREMAP.INC: cluster adjustment).
;
; A capacity is held in SECTORS and not in bytes, which is the other
; deviation.  cluster_count * sec_per_clus * bytes_per_sec overflows
; 32 bits at the top of FAT16's range (65524 clusters of 128 sectors
; is 4.29 thousand million bytes), and the comparison it is wanted
; for - "does what is in use still fit?" - is exact in sectors.
;=====================================================================

;--- fv_read - parse the boot sector of the volume at EBX on disk AL.
;--- CF set if it is not a FAT volume this program can work on, and
;--- [fv_raw] = 1 when that is because there is no boot sector there
;--- at all: the sector reads, and it does not begin with a jump and
;--- end with the signature.
;---
;--- NO BOOT SECTOR IS NOT A BAD ONE.  A partition FORMAT has never been
;--- run on has none - fd_create writes the table and nothing inside the
;--- partition, so its first sector is whatever the disk held there:
;--- nothing, or the extended record of a chain since deleted, which has
;--- the signature and no jump.  There is nothing in it to keep.  A
;--- sector with both and a BPB this cannot use is somebody's volume,
;--- and stays refused.
;---
;--- THE FAT32 SHAPE IS THE SAME BPB with two of its fields moved out
;--- of the way: the sectors-per-FAT count is zero at 16h and real at
;--- 24h, and the root entry count is zero because there is no root
;--- DIRECTORY REGION - the root is a cluster chain in the data area
;--- like any other directory.  That second fact is why FAT32 turned
;--- out to cost so little here: a region of no sectors is a region
;--- that does not move, and the relocator already handled it.
fv_read:
        pushad
        mov     byte ptr [fv_valid],0
        mov     byte ptr [fv_raw],0
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        jc      fv_bad
        mov     esi,[sec_buf]
        cmp     word ptr [esi+1FEh],MBR_SIG
        jne     fv_none
        mov     al,[esi]
        cmp     al,0EBh                 ; JMP SHORT over the BPB
        je      fv_boot
        cmp     al,0E9h                 ; or JMP NEAR
        jne     fv_none
fv_boot:

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
        or      eax,eax
        jnz     fv_gotspf
        mov     eax,[esi+BPB_SPF32]     ; a FAT32 BPB says nothing in the
fv_gotspf:                              ; sixteen-bit field and puts the
        mov     [fv_spf],eax            ; real number at 24h
        movzx   eax,word ptr [esi+BPB_TOTSEC16]
        or      eax,eax
        jnz     fv_gottot
        mov     eax,[esi+BPB_TOTSEC32]  ; the 16-bit field is zero when the
fv_gottot:                              ; volume needs the 32-bit one
        mov     [fv_total],eax
        ; and three that cluster adjustment needs: the media byte, which
        ; a FAT made at another width carries in its first entry, and
        ; FAT32's live copy and root cluster (meaningless on the others)
        movzx   eax,byte ptr [esi+BPB_MEDIA]
        mov     [fv_media],eax
        movzx   eax,word ptr [esi+BPB_EXTFLAGS]
        mov     [fv_extflags],eax
        mov     eax,[esi+BPB_ROOTCLUS]
        mov     [fv_rootclus],eax

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
        je      fv_bad                  ; neither field has a number in it,
                                        ; so this is not a FAT volume
        call    fv_derive
        cmp     dword ptr [fv_clusters],FAT32_MAXCLUS
        ja      fv_bad                  ; more clusters than FAT32 can number
        mov     byte ptr [fv_valid],1
        popad
        clc
        ret
fv_none:
        mov     byte ptr [fv_raw],1     ; never formatted
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
        mov     dword ptr [fv_nbad],0
        cmp     byte ptr [fv_valid],0
        je      fs_out

        mov     [fv_drive],al
        add     ebx,[fv_rsvd]           ; the first FAT
        mov     [fv_fatlba],ebx

        cmp     dword ptr [fv_bits],32
        je      fs_fat32
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
        cmp     word ptr [esi+ecx*2],0FFF7h
        jne     fs16_step
        inc     dword ptr [fv_nbad]     ; marked bad: larger clusters would
fs16_step:                              ; have to hold one
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
        cmp     eax,0FF7h
        jne     fs12_step
        inc     dword ptr [fv_nbad]
fs12_step:
        inc     ebp
        jmp     short fs12_one

;--- FAT32: 128 four-byte entries a sector, streamed the same way -
;--- and the top FOUR BITS OF EVERY ENTRY ARE RESERVED.  A scan that
;--- did not mask them would read a free cluster as used on any volume
;--- whose driver had ever set one, and the answer this routine gives
;--- is the floor on how far a shrink may go.
fs_fat32:
        xor     ebp,ebp                 ; EBP = the FAT sector
fs32_sec:
        cmp     ebp,[fv_spf]
        jae     fs_out
        mov     al,[fv_drive]
        mov     ebx,[fv_fatlba]
        add     ebx,ebp
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        jc      fs_out
        mov     esi,[sec_buf]
        xor     ecx,ecx                 ; ECX = the entry within the sector
fs32_ent:
        mov     eax,ebp
        shl     eax,7                   ; sector * 128
        add     eax,ecx                 ; = the cluster number
        cmp     eax,2
        jb      fs32_step
        mov     edx,[fv_clusters]
        inc     edx
        cmp     eax,edx
        ja      fs_out                  ; past the last cluster there is
        mov     edx,[esi+ecx*4]
        and     edx,0FFFFFFFh
        jz      fs32_step
        mov     [fv_usedhigh],eax
        cmp     edx,0FFFFFF7h
        jne     fs32_step
        inc     dword ptr [fv_nbad]
fs32_step:
        inc     ecx
        cmp     ecx,128
        jb      fs32_ent
        inc     ebp
        jmp     fs32_sec

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
        ja      fg_is32
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
fg_is32:
        ; MORE CLUSTERS THAN FAT16 CAN NUMBER, which used to be the end
        ; of the conversation.  It is the same second pass FAT12 gets
        ; above and for the same reason: the first estimate was made
        ; with two-byte entries, and a FAT sized for the wrong entry
        ; width is a lie about the volume.
        ;
        ; AND FAT32 HAS NO ROOT-DIRECTORY REGION.  Its root is an
        ; ordinary cluster chain in the data area, which is why this
        ; one line is the whole of what the relocator has to be told
        ; about it: a region of no sectors is a region that does not
        ; move.
        mov     dword ptr [fgw_bits],32
        mov     dword ptr [fgw_rootsec],0
        mov     eax,32
        call    fg_estspf
        or      eax,eax
        jz      fg_no
        mov     [fgw_spf],eax
        call    fg_derive
        jc      fg_no
        cmp     dword ptr [fgw_clus],FAT32_MAXCLUS
        ja      fg_no
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
        cmp     dword ptr [fgw_bits],32
        je      fn_32
        shl     eax,1                   ; two bytes an entry
        jmp     short fn_secs
fn_32:
        shl     eax,2                   ; four
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
        cmp     ebx,32
        je      fg_est32
        cmp     ebx,16
        jne     fg_est12
        mov     eax,[fgw_spc]
        shl     eax,8                   ; 256 * spc + FATs, and NOTHING
        add     eax,[fv_nfats]          ; else - see the derivation above
        jmp     short fg_esthave
fg_est32:
        mov     eax,[fgw_spc]
        shl     eax,7                   ; 128 * spc + FATs: an entry is four
        add     eax,[fv_nfats]          ; bytes, so 128 of them fit a sector
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
;   REMAP      no cluster size up to 32K expresses the new size
;   FATTYPE    the new size needs FAT32 from FAT16, or the other way
;   BADCLUS    the clusters would have to grow and some are marked bad
;   DAMAGED    the cluster size has to change and a chain or directory
;              does not hold together
;   OVERLAP    it would run into another partition
;   BOUNDS     it would run off the disk, or over the MBR
;
; A NEW SIZE THE CLUSTER SIZE CANNOT EXPRESS is no longer the end of
; it.  The current cluster size is still kept whenever it can be, and
; when it cannot, rm_choose finds the nearest that can - FAT12 and
; FAT16 trading places at the same size, or clusters halved or doubled
; - and the refusals above are what is left when nothing does.
;
; A PARTITION NOBODY HAS FORMATTED is planned too, and [pl_raw] says
; so.  It has no volume, so the rules about the volume have nothing to
; be about: OVERLAP and BOUNDS still hold, and the change is its table
; entry and nothing else.
;
; SO IS THE EXTENDED CONTAINER, and [pl_cont] says so.  What it holds
; is a chain, not a volume, and the one rule of its own is HOLDS: the
; chain has to fit in the new length.  A move takes the whole chain
; with it - every offset in the chain is relative, so not one of them
; changes - and a resize moves nothing at all.
;=====================================================================
rl_plan:
        pushad
        mov     dword ptr [pl_status],RL_OK
        mov     dword ptr [pl_op],OP_NONE
        mov     byte ptr [pl_same],0
        mov     byte ptr [pl_crossbl],0
        mov     byte ptr [pl_keepok],0
        mov     byte ptr [pl_raw],0
        mov     byte ptr [pl_cont],0
        mov     dword ptr [pl_cxn],0
        mov     dword ptr [pl_cxend],0
        mov     byte ptr [rm_mode],RM_NONE

        mov     esi,[pl_ent]
        mov     eax,[esi+PE_START]
        add     eax,[pl_base]           ; and only a logical drive has one
        mov     [pl_ostart],eax
        mov     eax,[esi+PE_SECS]
        mov     [pl_olen],eax
        mov     byte ptr [pl_boot],0
        cmp     byte ptr [esi+PE_BOOT],80h
        jne     rp_notboot
        mov     byte ptr [pl_boot],1
rp_notboot:
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    fd_isext
        jc      rp_notcont
        call    rl_cxload               ; the chain it holds, and how far
        jc      rp_chain                ; into it the chain reaches
        mov     byte ptr [pl_cont],1
        jmp     short rp_asked
rp_notcont:
        call    rl_isfat
        jc      rp_notfat

;--- the volume as it stands -----------------------------------------
        mov     al,[pl_drive]
        mov     ebx,[pl_ostart]
        call    fv_read
        jnc     rp_isvol
        cmp     byte ptr [fv_raw],0
        je      rp_notfat
        mov     byte ptr [pl_raw],1     ; not formatted: nothing in it to keep
rp_isvol:
        mov     al,[pl_drive]
        mov     ebx,[pl_ostart]
        call    fv_scanused             ; which finds nothing when there is
                                        ; no volume

;--- nothing asked for is not an error -------------------------------
rp_asked:
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
; THE REGION, WHICH IS NOT ALWAYS THE DISK.  A primary partition may
; go anywhere on the disk except sector zero, which is the master
; boot record and belongs to nobody; a logical drive may go anywhere
; inside its container and nowhere outside it, because outside it the
; table says the space is something else.  fd_entptr has already said
; which of those two this is.
        mov     eax,[pl_nstart]
        cmp     eax,[pl_lo]
        jb      rp_bounds
        cmp     dword ptr [pl_nlen],0
        je      rp_bounds
        add     eax,[pl_nlen]
        jc      rp_bounds               ; a length that wraps 4GB is not one
        cmp     eax,[pl_hi]
        ja      rp_bounds

;--- overlap with any of the walls fd_entptr found -------------------
        xor     ebp,ebp
        mov     esi,OFFSET nb_tbl
rp_ovl:
        cmp     ebp,[nb_n]
        jae     rp_noovl
        ; they overlap unless one ends at or before the other starts
        mov     eax,[esi+NB_START]
        add     eax,[esi+NB_SECS]       ; the wall's end
        mov     edx,[pl_nstart]
        cmp     edx,eax
        jae     rp_ovlstep              ; we start at or after its end
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]           ; our end
        mov     edx,[esi+NB_START]
        cmp     edx,eax
        jae     rp_ovlstep              ; it starts at or after our end
        mov     dword ptr [pl_status],RL_OVERLAP
        jmp     rp_out
rp_ovlstep:
        add     esi,NB_LEN
        inc     ebp
        jmp     short rp_ovl
rp_noovl:
        cmp     byte ptr [pl_cont],0
        je      rp_notcx
        mov     eax,[pl_cxend]          ; the container: the chain in it has
        sub     eax,[pl_ostart]         ; to fit, and that is all
        cmp     eax,[pl_nlen]
        ja      rp_holds
        jmp     rp_ok
rp_notcx:
        cmp     byte ptr [pl_raw],0
        jne     rp_boot                 ; no volume, so no layout to keep

;--- the geometry the specification would prefer, for the report -----
        mov     eax,[pl_nlen]
        mov     edi,OFFSET fg_ideal
        call    fg_smallest

;--- a move keeps the layout the volume HAS --------------------------
; Not the one fg_forspc would choose for its size.  A move copies the
; FATs as they are, and rl_wrboot writes fg_exec's FAT size into the
; boot sector it has moved - so fg_exec has to be what is there.  It
; was fg_forspc's, and a volume FORMAT had given a longer FAT than
; that came out of a move with a boot sector putting its second FAT,
; its root and its data in the wrong place: no files, every cluster
; lost.
        cmp     dword ptr [pl_op],OP_MOVE
        jne     rp_keep
        mov     dword ptr [fg_exec+FG_VALID],1
        mov     eax,[fv_bits]
        mov     dword ptr [fg_exec+FG_BITS],eax
        mov     eax,[fv_spc]
        mov     dword ptr [fg_exec+FG_SPC],eax
        mov     eax,[fv_spf]
        mov     dword ptr [fg_exec+FG_SPF],eax
        mov     eax,[fv_clusters]
        mov     dword ptr [fg_exec+FG_CLUS],eax
        mov     eax,[fv_datasec]
        mov     dword ptr [fg_exec+FG_DATA],eax
        mov     eax,[fv_rootsec]
        mov     dword ptr [fg_exec+FG_ROOT],eax
        mov     eax,[fv_clusters]
        imul    eax,[fv_spc]
        mov     dword ptr [fg_exec+FG_CAP],eax
        jmp     rp_boot

;--- and the geometry this program will actually execute -------------
; THE CURRENT CLUSTER SIZE, kept - see the deviation note in the FAT
; volume layer.  It is the regime in which cluster NUMBERS survive,
; which is what makes the operation non-destructive: no file's data
; moves relative to the data region and no directory entry has to be
; touched.
rp_keep:
        mov     eax,[pl_nlen]
        mov     ecx,[fv_spc]
        mov     edi,OFFSET fg_keep
        call    fg_forspc
        mov     eax,RL_REMAP            ; (MOV leaves the carry alone)
        jc      rp_adjust
        cmp     dword ptr [fg_keep+FG_VALID],0
        je      rp_adjust
        mov     edx,dword ptr [fg_keep+FG_BITS]
        mov     eax,RL_FATTYPE
        cmp     edx,[fv_bits]
        jne     rp_adjust
        mov     eax,dword ptr [fg_keep+FG_CAP]
        cmp     eax,[fv_usedsec]
        jb      rp_toosmall

        mov     byte ptr [pl_keepok],1
        mov     byte ptr [pl_same],1
        mov     esi,OFFSET fg_keep      ; exec = keep
        mov     edi,OFFSET fg_exec
        mov     ecx,FG_LEN / 4
        rep     movsd
        jmp     short rp_boot

;--- and when it cannot be kept --------------------------------------
; CLUSTER ADJUSTMENT: the nearest cluster size and width that do
; express the new size, and the volume read through and checked for
; it.  EAX is the refusal keeping the cluster size earned, which is
; what stands when nothing nearer does better.
rp_adjust:
        call    rm_choose
        jc      rp_refuse
        call    rm_prepare
        jc      rp_refuse

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
rp_refuse:                              ; EAX = whichever it was
        mov     [pl_status],eax
        jmp     short rp_out
rp_toosmall:
        mov     dword ptr [pl_status],RL_TOOSMALL
        jmp     short rp_out
rp_holds:
        mov     dword ptr [pl_status],RL_HOLDS
        jmp     short rp_out
rp_chain:
        mov     dword ptr [pl_status],RL_CHAIN
rp_out:
        popad
        ret

;--- rl_cxload - the chain inside the container being planned: chn_*,
;--- [pl_cxn] = how many logical drives it holds, and [pl_cxend] = one
;--- past the last sector any link of it uses.  CF set when the chain
;--- does not read cleanly, or belongs to another container.
;---
;--- A LINK CARRYING NOTHING AND LEADING NOWHERE USES NOTHING, which is
;--- fd_lgap's rule: it is the head an emptied container keeps, and a
;--- container holding only that is a container holding nothing.
rl_cxload:
        pushad
        call    fd_chain
        jc      rcl_bad
        cmp     dword ptr [chn_st],CHN_OK
        jne     rcl_bad
        mov     eax,[chn_slot]
        cmp     eax,[pl_idx]
        jne     rcl_bad                 ; a second container: no DOS ever
                                        ; followed it, and nor does this
        mov     eax,[chn_base]
        mov     [pl_cxend],eax
        mov     esi,[chn_tbl]
        xor     ebp,ebp
rcl_one:
        cmp     ebp,[chn_n]
        jae     rcl_ok
        cmp     dword ptr [esi+CH_VSTART],0
        je      rcl_novol
        inc     dword ptr [pl_cxn]
        jmp     short rcl_take
rcl_novol:
        cmp     dword ptr [esi+CH_NEXT],0
        je      rcl_next
rcl_take:
        mov     eax,[esi+CH_EBR]
        add     eax,[esi+CH_EXTENT]
        cmp     eax,[pl_cxend]
        jbe     rcl_next
        mov     [pl_cxend],eax
rcl_next:
        add     esi,CH_LEN
        inc     ebp
        jmp     short rcl_one
rcl_ok:
        popad
        clc
        ret
rcl_bad:
        popad
        stc
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
        cmp     al,PT_FAT32
        je      rif_yes
        cmp     al,PT_FAT32L
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
;            -> EAX = how many clusters it would have at the cluster
;               size and FAT width in rl_spc and rl_bits.  Zero when
;               the size cannot hold even its own overhead.
;
; The FAT width is held fixed rather than reclassified, and each limit
; is searched for one cluster size and width at a time: a size whose
; cluster count crosses into the other width is a different candidate,
; and the planner reaches it the same way - one candidate at a time,
; nearest first (rm_choose).
;=====================================================================
rl_ccfor:
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     [fgw_total],eax
        mov     eax,[rl_spc]
        mov     [fgw_spc],eax
        mov     eax,[fv_rootent]
        shl     eax,5
        add     eax,[fv_bps]
        dec     eax
        xor     edx,edx
        div     dword ptr [fv_bps]
        mov     [fgw_rootsec],eax
        mov     eax,[rl_bits]
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

;--- rl_minsize - the smallest the volume can be made: at its own
;--- cluster size or a SMALLER one, at either width of its family.
;--- EBX = the largest size worth considering (the search's ceiling).
;--- -> EAX, or zero when even the ceiling will not do.
;---
;--- SMALLER ONLY, because those keep everything where it is: a halved
;--- cluster is two new ones in the same sectors.  A larger cluster
;--- would pack the files and could go further, but packing to make
;--- room is a defragmenter's answer, and this is a resize.
rl_minsize:
        push    ebp
        push    esi
        mov     dword ptr [rlm_best],0
        mov     ebp,[fv_spc]            ; EBP = a cluster size, halving
rlm_spc:
        mov     [rl_spc],ebp
        mov     esi,12                  ; ESI = a width of the family
        cmp     dword ptr [fv_bits],32
        jne     rlm_bits
        mov     esi,32
rlm_bits:
        mov     [rl_bits],esi
        call    rl_min1
        or      eax,eax
        jz      rlm_bnext
        cmp     dword ptr [rlm_best],0
        je      rlm_take
        cmp     eax,[rlm_best]
        jae     rlm_bnext
rlm_take:
        mov     [rlm_best],eax
rlm_bnext:
        cmp     esi,16
        jae     rlm_snext
        mov     esi,16
        jmp     short rlm_bits
rlm_snext:
        shr     ebp,1
        jnz     rlm_spc
        mov     eax,[rlm_best]
        pop     esi
        pop     ebp
        ret

;--- rl_maxsize - the largest the volume can be made: at its own
;--- cluster size or a LARGER one, up to 32K and not at all when some
;--- clusters are marked bad, at either width of its family.  EBX =
;--- the ceiling that the disk and the neighbouring partitions impose.
;--- -> EAX.
rl_maxsize:
        push    ebp
        push    esi
        mov     dword ptr [rlm_best],0
        mov     ebp,[fv_spc]            ; EBP = a cluster size, doubling
rlx_spc:
        mov     [rl_spc],ebp
        mov     esi,12
        cmp     dword ptr [fv_bits],32
        jne     rlx_bits
        mov     esi,32
rlx_bits:
        mov     [rl_bits],esi
        call    rl_max1
        cmp     eax,[rlm_best]
        jbe     rlx_bnext
        mov     [rlm_best],eax
rlx_bnext:
        cmp     esi,16
        jae     rlx_snext
        mov     esi,16
        jmp     short rlx_bits
rlx_snext:
        cmp     dword ptr [fv_nbad],0
        jne     rlx_done
        shl     ebp,1
        cmp     ebp,RM_SPCMAX
        jbe     rlx_spc
rlx_done:
        mov     eax,[rlm_best]
        pop     esi
        pop     ebp
        ret

;--- rl_needs - for rl_spc and rl_bits: rl_needcc = the fewest clusters
;--- that hold what is stored and are a volume of that width, rl_maxcc =
;--- the most that width numbers.
;---
;--- WHAT IS STORED STAYS WHERE IT IS at this cluster size or a smaller
;--- one, so the clusters have to reach the last sector in use.  A
;--- larger one packs the files from the front, and the plan measures
;--- that; here it needs nothing but the floor.
;---
;--- AND THE WIDTH HAS A FLOOR OF ITS OWN.  The type of a FAT volume is
;--- its CLUSTER COUNT and nothing else, so a size below the bottom of
;--- a width's band is not a volume of that width.  It showed up first
;--- on FAT32, where the floor is 65525 clusters.
rl_needs:
        push    eax
        push    ecx
        push    edx
        mov     eax,1                   ; a volume must have at least one
        mov     ecx,[rl_spc]
        cmp     ecx,[fv_spc]
        ja      rns_have
        mov     eax,[fv_usedsec]
        xor     edx,edx
        div     ecx                     ; clusters needed for what is stored
        or      edx,edx
        jz      rns_exact
        inc     eax                     ; round up: a part-used cluster is used
rns_exact:
        cmp     eax,1
        jae     rns_have
        mov     eax,1
rns_have:
        mov     ecx,1
        mov     edx,FAT12_MAXCLUS
        cmp     dword ptr [rl_bits],12
        je      rns_floor
        mov     ecx,FAT12_MAXCLUS + 1
        mov     edx,FAT16_MAXCLUS
        cmp     dword ptr [rl_bits],16
        je      rns_floor
        mov     ecx,FAT16_MAXCLUS + 1
        mov     edx,FAT32_MAXCLUS
rns_floor:
        cmp     eax,ecx
        jae     rns_out
        mov     eax,ecx
rns_out:
        mov     [rl_needcc],eax
        mov     [rl_maxcc],edx
        pop     edx
        pop     ecx
        pop     eax
        ret

;--- rl_min1 - the smallest volume at rl_spc and rl_bits that holds what
;--- is stored.  EBX = the ceiling.  -> EAX, or zero when there is none.
;---
;--- BINARY SEARCH, and it is allowed to be one because the cluster
;--- count is monotonic in the volume size: a bigger volume never has
;--- fewer clusters.  The predicate rl_plan applies is NOT monotonic
;--- (growing a FAT12 volume far enough makes it a FAT16 one), which is
;--- why the search is on the CLUSTER COUNT and the width is pinned -
;--- searching the predicate directly would converge on the wrong side
;--- of that boundary.
rl_min1:
        push    ecx
        push    edx
        push    esi
        push    edi
        call    rl_needs
        mov     eax,[rl_needcc]
        cmp     eax,[rl_maxcc]
        ja      rms_none                ; that width cannot hold it at all

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
        jb      rms_none                ; even the ceiling was not enough
        cmp     ecx,[rl_maxcc]
        jbe     rms_out
rms_none:
        xor     eax,eax
rms_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        ret

;--- rl_max1 - the largest volume at rl_spc and rl_bits, no bigger than
;--- EBX.  -> EAX, or zero when no size up to there is one.
rl_max1:
        push    ecx
        push    edx
        push    esi
        push    edi
        call    rl_needs

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
        ; and the size found has to be a volume of that width at all,
        ; holding what is stored: the largest FAT16 volume a ceiling
        ; allows is no answer when the ceiling is too low for FAT16
        push    eax
        call    rl_ccfor
        mov     ecx,eax
        pop     eax
        cmp     ecx,[rl_needcc]
        jae     rmx_out
        xor     eax,eax
rmx_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        ret

;--- rl_room - how far the partition being planned could extend before
;--- it met something.  -> EAX = the largest length it could have at
;--- its current start.
rl_room:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    ebp
        mov     ebx,[pl_hi]             ; the end of the region, until
                                        ; something nearer turns up
        xor     ebp,ebp
        mov     esi,OFFSET nb_tbl
rr_scan:
        cmp     ebp,[nb_n]
        jae     rr_done
        mov     eax,[esi+NB_START]
        cmp     eax,[pl_ostart]
        jbe     rr_step                 ; it is behind us, so it is not a wall
        cmp     eax,ebx
        jae     rr_step
        mov     ebx,eax                 ; the nearest thing in front
rr_step:
        add     esi,NB_LEN
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

        call    fd_entptr

;--- plan a change of nothing, which is how the volume gets parsed ---
; rl_plan reads the BPB and scans the FAT before it looks at what was
; asked for, so asking it for no change at all is the cheapest way to
; get the volume's own state without a second path that could drift
; out of agreement with the one the executor uses.
        mov     esi,[pl_ent]
        mov     eax,[esi+PE_START]
        add     eax,[pl_base]
        mov     [pl_nstart],eax
        mov     eax,[esi+PE_SECS]
        mov     [pl_nlen],eax
        call    rl_plan
        cmp     dword ptr [pl_status],RL_NOTFAT
        je      fdp_notfat
        cmp     dword ptr [pl_status],RL_CHAIN
        je      fdp_chbad

;--- the heading -----------------------------------------------------
        call    crlf
        mov     edx,OFFSET msg_disk
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_part
        call    puts
        call    fd_putnum
        mov     edx,OFFSET msg_sp3
        call    puts
        mov     esi,[pl_ent]
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
        cmp     byte ptr [pl_cont],0
        jne     fdp_cx
        cmp     byte ptr [pl_raw],0
        jne     fdp_raw

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
        mov     [pl_minsz],eax          ; and kept, for the screen's resize
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
        mov     [pl_maxsz],eax
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

;--- one nobody has formatted ----------------------------------------
; NOTHING IS STORED IN IT, so the only limits are the room in front of
; it and the cylinder it starts in: it can shrink to the first boundary
; past its start, and grow into all of the room.
fdp_raw:
        mov     edx,OFFSET msg_vol
        call    puts
        mov     edx,OFFSET msg_raw
        call    puts
        jmp     short fdp_free

;--- the extended container ------------------------------------------
; THE SAME LIMITS, with one more: it cannot end before its chain does,
; which rl_plan has measured as pl_cxend.
fdp_cx:
        mov     edx,OFFSET msg_vol
        call    puts
        mov     edx,OFFSET msg_cxnone
        mov     eax,[pl_cxn]
        or      eax,eax
        jz      fdx_said
        mov     edx,OFFSET msg_cxsome
        call    puts
        call    put1
        mov     edx,OFFSET msg_cxlast
        call    puts
        mov     eax,[pl_cxend]
        call    put1
        jmp     short fdp_free
fdx_said:
        call    puts

fdp_free:
        call    rl_room
        mov     [pl_room],eax
        mov     edx,OFFSET msg_room
        call    puts
        call    put10
        call    fdp_mb

        mov     eax,[pl_ostart]
        inc     eax
        call    fd_spcyl                ; ECX = a cylinder
        call    fd_cylup
        cmp     eax,[pl_cxend]          ; zero unless it is the container
        jae     fdr_end
        mov     eax,[pl_cxend]
fdr_end:
        sub     eax,[pl_ostart]
        cmp     eax,[pl_olen]
        jbe     fdr_min
        mov     eax,[pl_olen]           ; it is smaller than that already
fdr_min:
        mov     [pl_minsz],eax
        mov     edx,OFFSET msg_shrink
        call    puts
        call    put10
        call    fdp_mb

        mov     eax,[pl_room]
        mov     [pl_maxsz],eax
        mov     edx,OFFSET msg_grow
        call    puts
        call    put10
        call    fdp_mb
        jmp     fdp_done

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
fdp_chbad:
        mov     edx,OFFSET msg_chbad
        call    puts
        jmp     fd_fail
fdp_notfat:
        mov     edx,OFFSET msg_notfat
        call    puts
        jmp     fd_fail

;=====================================================================
; fd_getdp - the first argument, as <disk>:<partition>.
;
;   -> [pl_drive] = a BIOS drive number, and either [pl_idx] = a
;      primary slot 0-3 with [pl_log] = 0, or [pl_log] = a 1-based
;      logical drive inside the extended chain
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
        mov     dword ptr [pl_log],0
        cmp     eax,PART_MAX
        jbe     fgd_primary
        ; ABOVE FOUR IS A LOGICAL DRIVE, numbered on from the primary
        ; slots, so 5 is the first drive in the extended chain.  It is
        ; the numbering every tool since the 1990s has used and, more
        ; to the point, the numbering the report above prints - which
        ; is where the number in the command line comes from.
        ;
        ; NOTHING IS CHECKED AGAINST THE CHAIN HERE.  This runs before
        ; any disk has been read, and "there is no sixth partition" is
        ; a sentence a verb says once it has looked, not a usage
        ; screen about how to type an argument that was typed right.
        sub     eax,PART_MAX
        cmp     eax,EBR_MAX
        ja      fgd_bad
        mov     [pl_log],eax
        mov     dword ptr [pl_idx],0
        jmp     short fgd_tail
fgd_primary:
        dec     eax
        mov     [pl_idx],eax
fgd_tail:
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
; everything: every chain, and every directory entry.  SAFEPART has
; that code and refuses to run it against a live volume because it has
; never been proven against disk images.  This program has its own,
; FDREMAP.INC, reached only when the cluster size CANNOT be kept, and
; only after the whole volume has been read through and found to hold
; together - see that file for why each of its steps is safe.
;
; NOTHING HERE RUNS UNARMED.  The screen's dry run clears dk_armed, so
; it walks the whole planner, says exactly what it would have done,
; and is physically incapable of changing a sector.
;=====================================================================
rl_exec:
        pushad
        cmp     byte ptr [dk_armed],0
        je      rx_notarmed
        cmp     dword ptr [pl_status],RL_OK
        jne     rx_badplan

        ; THE CONTAINER CARRIES ITS CHAIN, and then its entry is written
        ; like any other.  A move and a resize at once is safe here as
        ; well: what is copied is the chain, which keeps its length.
        cmp     byte ptr [pl_cont],0
        je      rx_notcx
        call    rl_cxmove
        jc      rx_ioerr
        jmp     rx_table
rx_notcx:
        ; UNFORMATTED, AND SO NOTHING TO CARRY.  There is no volume to
        ; copy or re-lay, so a move or a resize - or both at once, which
        ; is refused below only because of the copy - is the entry.
        cmp     byte ptr [pl_raw],0
        jne     rx_table

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

; STAGED FOR THE SCREEN'S COMMIT, THE ENTRY IS ALL THAT IS KEPT.  The
; volume's data is far more than can be held and nothing reads it until
; the commit runs this again for real - and FDPEND.INC locks the volume
; until then, because its boot sector and FAT are still where they were.
rx_move:
        cmp     byte ptr [dk_armed],DK_STAGE
        je      rx_table
        call    rl_domove
        jc      rx_ioerr
        jmp     short rx_table
rx_resize:
        cmp     byte ptr [dk_armed],DK_STAGE
        je      rx_table
        cmp     byte ptr [rm_mode],RM_NONE
        je      rx_samelayout
        call    rm_exec                 ; the clusters change too
        jc      rx_ioerr
        jmp     short rx_table
rx_samelayout:
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
        call    rl_typebyte             ; -> AL
        ; ONLY THIS ENTRY.  SAFEPART repacks all four CHS fields,
        ; which quietly "corrects" tables it was never asked about -
        ; and a partition editor that rewrites a neighbour's bytes as
        ; a side effect of touching this one has done something the
        ; user cannot see and did not ask for.  PE_BOOT is left alone
        ; for the same reason: an active partition that is moved is
        ; still the active one.
        mov     edi,[pl_ent]
        mov     ebx,[pl_nstart]
        mov     ecx,[pl_nlen]
        mov     edx,[pl_base]
        call    fd_putent

        ; and the sector it lives in, which for a logical drive is its
        ; own boot record and not the master one
        mov     edi,[mbr_buf]
        cmp     dword ptr [pl_sec],0
        je      rx_wrsec
        mov     edi,[ebr_buf]
rx_wrsec:
        mov     al,[pl_drive]
        mov     ebx,[pl_sec]
        mov     ecx,1
        call    dk_write
        jc      rx_ioerr
        call    rx_fixprev
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

;---------------------------------------------------------------------
; rl_cxmove - the chain, carried to where the container now starts.
;             Nothing to do for a resize.  CF on a failed read or write.
;
; ONE COPY OF THE WHOLE SPAN THE CHAIN USES, from the head record to
; the end of the last link, and rl_copy picks the direction that makes
; an overlap safe.  Every start in the chain is an offset - a volume's
; from its own record, a link's from the head - so the copy is already
; a correct chain.  What is not relative is put right after it, link by
; link: the CHS fields, which name places on the disk; the LBA spelling
; of a type, for any that now ends past CHS's reach; and the hidden
; sectors of a volume that recorded where it was.
;
; AN EMPTY CONTAINER STILL CARRIES ITS FIRST SECTOR.  Whatever the head
; is - an empty record, or no record at all - is what the head must be
; at the new start too, and not whatever that sector held before.
;
; STAGED, ONLY WHAT THE SCREEN READS IS CARRIED: the head, every record,
; and every volume's boot sector, which is what the link fix-ups below
; read back at the new place.  The drives' data is not, and FDPEND.INC
; locks them until the commit carries it.
;---------------------------------------------------------------------
rl_cxmove:
        pushad
        mov     eax,[pl_nstart]
        cmp     eax,[pl_ostart]
        je      rcm_ok
        mov     edx,OFFSET msg_movecx
        call    puts
        cmp     byte ptr [dk_armed],DK_STAGE
        jne     rcm_copy
        call    rcm_carry
        jc      rcm_bad
        jmp     short rcm_fix
rcm_copy:
        mov     eax,[pl_ostart]
        mov     [rc_src],eax
        mov     eax,[pl_nstart]
        mov     [rc_dst],eax
        mov     eax,[pl_cxend]
        sub     eax,[pl_ostart]
        jnz     rcm_count
        inc     eax                     ; the head, at least
rcm_count:
        mov     [rc_cnt],eax
        call    rl_copy
        jc      rcm_bad

rcm_fix:
        mov     edx,OFFSET msg_fixchain
        call    puts
        mov     esi,[chn_tbl]
        xor     ebp,ebp
rcm_link:
        cmp     ebp,[chn_n]
        jae     rcm_ok
        call    rcm_fixlink
        jc      rcm_bad
        add     esi,CH_LEN
        inc     ebp
        jmp     short rcm_link
rcm_ok:
        popad
        clc
        ret
rcm_bad:
        popad
        stc
        ret

;--- rcm_carry - staged: the head, every record and every volume's boot
;--- sector of the chain, put where the move takes them.  ALL ARE READ
;--- BEFORE ANY IS WRITTEN, because the old place and the new can
;--- overlap and a staged sector is what the next read finds.  cpy_buf
;--- holds them; a chain with more than it holds cannot be staged.  CF
;--- on failure.
rcm_carry:
        pushad
        mov     byte ptr [rcm_write],0
        call    rcm_pass
        jc      rcy_bad
        mov     byte ptr [rcm_write],1
        call    rcm_pass
        jc      rcy_bad
        popad
        clc
        ret
rcy_bad:
        popad
        stc
        ret

;--- rcm_pass - each sector rcm_carry carries, in the same order both
;--- times: read into cpy_buf, or written from it at the new place
rcm_pass:
        pushad
        mov     edi,[cpy_buf]
        mov     dword ptr [rcm_left],65536 / SEC_BYTES
        mov     ebx,[pl_ostart]
        call    rcm_one                 ; the head
        jc      rcp_bad
        mov     esi,[chn_tbl]
        xor     ebp,ebp
rcp_link:
        cmp     ebp,[chn_n]
        jae     rcp_ok
        mov     ebx,[esi+CH_EBR]
        cmp     ebx,[pl_ostart]
        je      rcp_vol                 ; the head, carried already
        call    rcm_one
        jc      rcp_bad
rcp_vol:
        mov     ebx,[esi+CH_VSTART]
        or      ebx,ebx
        jz      rcp_next
        call    rcm_one
        jc      rcp_bad
rcp_next:
        add     esi,CH_LEN
        inc     ebp
        jmp     short rcp_link
rcp_ok:
        popad
        clc
        ret
rcp_bad:
        popad
        stc
        ret

;--- rcm_one - EBX = a sector where the chain is now, EDI -> its place
;--- in cpy_buf, moved on past it.  CF on failure.
rcm_one:
        cmp     dword ptr [rcm_left],0
        je      rco_full
        dec     dword ptr [rcm_left]
        cmp     byte ptr [rcm_write],0
        jne     rco_put
        call    fd_secrd
        jmp     short rco_next
rco_put:
        sub     ebx,[pl_ostart]
        add     ebx,[pl_nstart]
        call    fd_secwr
rco_next:
        lea     edi,[edi+SEC_BYTES]     ; LEA, so the carry survives
        ret
rco_full:
        stc
        ret

;--- rcm_fixlink - ESI -> a link as fd_chain read it, at its OLD place.
;--- Its record where it is now, with both entries' CHS worked out
;--- again, and its volume's boot sector if that names its old start.
rcm_fixlink:
        pushad
        mov     ebx,[esi+CH_EBR]
        sub     ebx,[pl_ostart]
        add     ebx,[pl_nstart]
        mov     [cx_ebr],ebx
        mov     edi,[eb2_buf]
        call    fd_secrd
        jc      rfl_bad
        mov     edi,[eb2_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     rfl_ok                  ; an empty head with no record in it
        add     edi,PART_OFF
        mov     edx,[cx_ebr]            ; slot 0 is measured from its record
        call    rfl_entry
        add     edi,PE_LEN
        mov     edx,[pl_nstart]         ; and slot 1 from the head
        call    rfl_entry
        mov     ebx,[cx_ebr]
        mov     edi,[eb2_buf]
        call    fd_secwr
        jc      rfl_bad
        mov     eax,[esi+CH_VSTART]
        or      eax,eax
        jz      rfl_ok
        call    rfl_hidden
        jc      rfl_bad
rfl_ok:
        popad
        clc
        ret
rfl_bad:
        popad
        stc
        ret

;--- rfl_entry - EDI -> an entry, EDX = what its start is measured from.
;--- The same start and length, with CHS and the type for where it is.
rfl_entry:
        pushad
        cmp     byte ptr [edi+PE_TYPE],PT_EMPTY
        je      rfe_out
        mov     ebx,[edi+PE_START]
        add     ebx,edx                 ; the absolute first sector
        mov     ecx,[edi+PE_SECS]
        mov     al,[edi+PE_TYPE]
        call    fd_lbatype
        call    fd_putent
rfe_out:
        popad
        ret

;--- rfl_hidden - EAX = a volume's old start.  Its boot sector, where it
;--- is now, gets the new start as its hidden sectors - but only if the
;--- old start is what it says.  A volume that counts from its own
;--- record, as some systems write them, is still right, and is left so.
rfl_hidden:
        pushad
        mov     [cx_vold],eax
        sub     eax,[pl_ostart]
        add     eax,[pl_nstart]
        mov     [cx_vnew],eax
        mov     ebx,eax
        mov     edi,[sec_buf]
        call    fd_secrd
        jc      rfh_bad
        mov     esi,[sec_buf]
        cmp     word ptr [esi+1FEh],MBR_SIG
        jne     rfh_ok                  ; not a boot sector
        mov     al,[esi]
        cmp     al,0EBh
        je      rfh_boot
        cmp     al,0E9h
        jne     rfh_ok
rfh_boot:
        mov     eax,[esi+BPB_HIDDSEC]
        cmp     eax,[cx_vold]
        jne     rfh_ok
        mov     eax,[cx_vnew]
        mov     [esi+BPB_HIDDSEC],eax
        mov     ebx,[cx_vnew]
        mov     edi,[sec_buf]
        call    fd_secwr
        jc      rfh_bad
        ; AND FAT32'S SPARE COPY, which says the same and is believed
        ; when the first is damaged - rw_backup's argument
        cmp     word ptr [esi+BPB_SPF16],0
        jne     rfh_ok
        movzx   ebx,word ptr [esi+BPB_BKBOOT]
        or      ebx,ebx
        jz      rfh_ok
        movzx   eax,word ptr [esi+BPB_RSVD]
        cmp     ebx,eax
        jae     rfh_ok                  ; FFFFh among them: there is none
        add     ebx,[cx_vnew]
        mov     edi,[sec_buf]
        call    fd_secwr
        jc      rfh_bad
rfh_ok:
        popad
        clc
        ret
rfh_bad:
        popad
        stc
        ret

;--- fd_lbatype - AL = a type, EBX = a first sector, ECX = a length.
;--- -> AL: the LBA spelling of the type when part of it lies past what
;--- CHS reaches, and the type as it was otherwise.  Only that way: an
;--- LBA type is right anywhere, and one that is already there is left.
fd_lbatype:
        push    edx
        mov     edx,ebx
        add     edx,ecx
        cmp     edx,[phy_chs]
        jbe     flt_out
        cmp     al,PT_FAT16S
        je      flt_16
        cmp     al,PT_FAT16B
        je      flt_16
        cmp     al,PT_FAT32
        je      flt_32
        cmp     al,PT_EXTENDED
        jne     flt_out
        mov     al,PT_EXTENDEDL
        jmp     short flt_out
flt_16:
        mov     al,PT_FAT16L
        jmp     short flt_out
flt_32:
        mov     al,PT_FAT32L
flt_out:
        pop     edx
        ret

;---------------------------------------------------------------------
; rx_fixprev - the pointer that NAMES this link, when the link has
;              changed size.  CF on a failed read or write.
;
; The slot-1 entry in the record before a logical drive carries the
; length of the whole link - its boot record, the gap in front of the
; volume, and the volume - and a move or a resize changes that length.
; NOTHING WALKS THE CHAIN BY THAT FIELD, which is exactly why it is
; the field that quietly rots: a table can be wrong here for years and
; every tool that reads it will still find every drive.  It is written
; because the next program to read it may not be one of those tools,
; and because a number that is nobody's business to check is a number
; nobody will notice is wrong.
;
; THE HEAD OF A CHAIN HAS NO SUCH POINTER.  What describes it is the
; container's own entry in the master boot record, and that entry
; describes the WHOLE container and not this one link - so there is
; nothing to correct, and enlarging the container to match a volume
; that grew would be a write to the master boot record that a resize
; has no business making.
;---------------------------------------------------------------------
rx_fixprev:
        pushad
        cmp     dword ptr [pl_log],0
        je      rxp_ok
        cmp     dword ptr [lg_idx],0
        je      rxp_ok
        imul    esi,dword ptr [lg_idx],CH_LEN
        add     esi,[chn_tbl]
        sub     esi,CH_LEN              ; the record in front of it
        mov     ebx,[esi+CH_EBR]
        mov     [lg_prev],ebx
        mov     edi,[eb2_buf]
        call    fd_secrd
        jc      rxp_bad
        mov     edi,[eb2_buf]
        add     edi,PART_OFF + PE_LEN
        mov     al,[chn_ctype]
        mov     ebx,[pl_sec]
        mov     ecx,[pl_nstart]
        add     ecx,[pl_nlen]
        sub     ecx,[pl_sec]            ; record, gap and volume together
        mov     edx,[chn_base]
        call    fd_putent
        mov     byte ptr [edi+PE_BOOT],0
        mov     ebx,[lg_prev]
        mov     edi,[eb2_buf]
        call    fd_secwr
        jc      rxp_bad
rxp_ok:
        popad
        clc
        ret
rxp_bad:
        popad
        stc
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
        call    rr_layout
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

;--- rr_layout - where the FATs, the root and the data are now, and
;--- where fg_exec puts them.  Cluster adjustment uses it too.
rr_layout:
        push    eax
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
        pop     eax
        ret

;--- the root directory, which never changes size --------------------
; UNLESS THERE IS NOT ONE.  FAT32 keeps its root in the data area as
; an ordinary cluster chain, so the region is zero sectors long and
; moves with the data like everything else in it - and saying "moving
; the root directory" about nothing would be the only line of this
; program that described work it had not done.
rz_root:
        push    eax
        cmp     dword ptr [fv_rootsec],0
        je      rz_noroot
        mov     edx,OFFSET msg_moveroot
        call    puts
        mov     eax,[rr_oroot]
        mov     [rc_src],eax
        mov     eax,[rr_nroot]
        mov     [rc_dst],eax
        mov     eax,[fv_rootsec]
        mov     [rc_cnt],eax
        call    rl_copy
rz_noroot:
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

        ; WHERE THE TEMPLATE COMES FROM, AND IT IS NOT ALWAYS WHERE THE
        ; VOLUME WAS.  A MOVE copies the whole volume, boot sector and
        ; all, so by the time this runs the sector to be patched is
        ; already sitting at the destination - and reading the OLD
        ; address back is wrong twice over: harmlessly on a move whose
        ; ranges do not overlap, and fatally on one where they do,
        ; because the copy has by then written something else into that
        ; very sector.  What came back was a BPB made of file data,
        ; and the volume it was written over stopped being a volume.
        ;
        ; A resize moves three regions and never touches the reserved
        ; area at all, so its template is still exactly where it has
        ; always been - and its start has not changed either.
        mov     ebx,[pl_ostart]
        cmp     dword ptr [pl_op],OP_MOVE
        jne     rw_haveold
        mov     ebx,[pl_nstart]
rw_haveold:
        mov     al,[pl_drive]
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
        ; SIXTEEN BITS OR THIRTY-TWO, and both fields written either
        ; way.  A FAT32 BPB says nothing in the sixteen-bit ones and
        ; puts its numbers at 20h and 24h; a FAT12 or FAT16 BPB has
        ; nothing at 24h at all.  Writing only the field that changed
        ; would leave whatever formatted the volume before in the
        ; other one - and an operating system reads the sixteen-bit
        ; field FIRST and believes it.
        cmp     dword ptr [fg_exec+FG_BITS],32
        je      rw_f32
        mov     eax,dword ptr [fg_exec+FG_SPF]
        mov     [edi+BPB_SPF16],ax
        jmp     short rw_geom
rw_f32:
        mov     word ptr [edi+BPB_SPF16],0
        mov     eax,dword ptr [fg_exec+FG_SPF]
        mov     [edi+BPB_SPF32],eax
        mov     word ptr [edi+BPB_TOTSEC16],0
        mov     eax,[pl_nlen]
        mov     [edi+BPB_TOTSEC32],eax
rw_geom:
        movzx   eax,word ptr [dsk_rec+DSK_SPT]
        mov     [edi+BPB_SECPERTRK],ax
        movzx   eax,word ptr [dsk_rec+DSK_HEADS]
        mov     [edi+BPB_NUMHEADS],ax

        ; THE CLUSTER SIZE, which only cluster adjustment changes - and
        ; with it FAT32's root directory, whose place is a cluster NUMBER
        mov     eax,dword ptr [fg_exec+FG_SPC]
        mov     [edi+BPB_SPC],al
        cmp     byte ptr [rm_mode],RM_SPLIT
        jb      rw_tname                ; the same numbers
        cmp     dword ptr [fg_exec+FG_BITS],32
        jne     rw_tname
        mov     eax,[edi+BPB_ROOTCLUS]
        call    rm_map
        mov     [edi+BPB_ROOTCLUS],eax
rw_tname:
        ; AND THE NAME OF THE WIDTH, which a FAT12 or FAT16 boot sector
        ; carries at 36h when its extended signature says the field is
        ; there.  Nothing is supposed to believe it - the cluster count
        ; decides - but plenty of things print it.
        cmp     byte ptr [rm_mode],RM_NONE
        je      rw_hidden
        cmp     dword ptr [fg_exec+FG_BITS],32
        je      rw_hidden
        cmp     byte ptr [edi+26h],29h
        jne     rw_hidden
        cmp     dword ptr [edi+36h],31544146h   ; "FAT1", and a digit
        jne     rw_hidden
        mov     al,'2'
        cmp     dword ptr [fg_exec+FG_BITS],12
        je      rw_tdigit
        mov     al,'6'
rw_tdigit:
        mov     [edi+3Ah],al
rw_hidden:

        ; hidden sectors: where the volume now begins, which is the
        ; one field a MOVE changes and the one thing inside the volume
        ; that knows where the volume is
        mov     eax,[pl_nstart]
        mov     [edi+BPB_HIDDSEC],eax

        ; BPB_ROOTCLUS IS NOT TOUCHED by a same-layout resize, and that
        ; is the whole same-layout argument in one field.  FAT32's root
        ; directory is a cluster chain, so where it is on the disk is a
        ; CLUSTER NUMBER - and cluster numbers survive, unless the
        ; clusters themselves changed, above.

        mov     al,[pl_drive]
        mov     ebx,[pl_nstart]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_write
        jc      rw_bad

        cmp     dword ptr [fg_exec+FG_BITS],32
        jne     rw_ok
        call    rw_backup
        jc      rw_bad
        call    rw_fsinfo
        jc      rw_bad
rw_ok:
        popad
        clc
        ret
rw_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; rw_backup - THE SPARE COPY OF THE BOOT SECTOR, which FAT32 has and
;             nothing else does.
;
; BPB_BKBOOT says where in the reserved area it lives - six, on every
; volume anything has ever formatted - and an operating system that
; finds the first one damaged reads that one instead.  So a resize
; that wrote one of the two and not the other would leave behind a
; spare copy of the volume's OLD size: a backup that is worse than no
; backup, because it will be believed.
;
; sec_buf still holds the sector that was just written, which is
; exactly what the copy is supposed to be.
;---------------------------------------------------------------------
rw_backup:
        pushad
        mov     edi,[sec_buf]
        movzx   ebx,word ptr [edi+BPB_BKBOOT]
        or      ebx,ebx
        jz      rwb_none                ; nought and FFFFh both mean there
        cmp     ebx,0FFFFh              ; is not one
        je      rwb_none
        cmp     ebx,[fv_rsvd]
        jae     rwb_none                ; it would be outside the reserved
                                        ; area, so it is not one either
        add     ebx,[pl_nstart]
        mov     al,[pl_drive]
        mov     ecx,1
        call    dk_write
        jc      rwb_bad
rwb_none:
        popad
        clc
        ret
rwb_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; rw_fsinfo - THE FREE-CLUSTER COUNT, which is now a lie.
;
; The volume has a different number of clusters than it had a moment
; ago, so the count cached in the FSInfo sector is wrong.  It is a
; HINT and not a record - the specification says so in as many words -
; and the value that means "I do not know, go and count" is all ones.
;
; Writing that is the honest answer and costs one pass over the FAT at
; the next mount.  Leaving the old number costs a free-space figure
; that is wrong until something notices, which on a volume that has
; just grown means an operating system that will not use the space it
; was given.
;
; The signatures are checked before anything is written, because a
; reserved sector that is not an FSInfo sector is somebody else's.
;---------------------------------------------------------------------
FSI_SIG1        EQU     41615252h       ; 'RRaA' at 0
FSI_SIG2        EQU     61417272h       ; 'rrAa' at 1E4h
FSI_FREE        EQU     1E8h
FSI_NEXT        EQU     1ECh

rw_fsinfo:
        pushad
        mov     edi,[sec_buf]
        movzx   ebp,word ptr [edi+BPB_FSINFO]
        or      ebp,ebp
        jz      rwf_none
        cmp     ebp,0FFFFh
        je      rwf_none
        cmp     ebp,[fv_rsvd]
        jae     rwf_none

        mov     ebx,ebp
        add     ebx,[pl_nstart]
        mov     al,[pl_drive]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        jc      rwf_none                ; it would not read, and a free-space
                                        ; hint is not worth failing a resize
        mov     edi,[sec_buf]           ; that has already been done
        cmp     dword ptr [edi],FSI_SIG1
        jne     rwf_none
        cmp     dword ptr [edi+1E4h],FSI_SIG2
        jne     rwf_none
        mov     dword ptr [edi+FSI_FREE],0FFFFFFFFh
        mov     dword ptr [edi+FSI_NEXT],0FFFFFFFFh
        mov     ebx,ebp
        add     ebx,[pl_nstart]
        mov     al,[pl_drive]
        mov     ecx,1
        call    dk_write
        jc      rwf_bad
rwf_none:
        popad
        clc
        ret
rwf_bad:
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
;
; A PARTITION THAT WAS NEVER FORMATTED has no FAT width to go by, so it
; gets what fd_create would give a new partition of its new shape.  THE
; CONTAINER keeps its own, spelled for LBA if it now reaches past CHS.
;=====================================================================
rl_typebyte:
        push    ebx
        cmp     byte ptr [pl_cont],0
        je      rf_notcx
        push    ecx
        push    esi
        mov     esi,[pl_ent]
        mov     al,[esi+PE_TYPE]
        mov     ebx,[pl_nstart]
        mov     ecx,[pl_nlen]
        call    fd_lbatype
        pop     esi
        pop     ecx
        pop     ebx
        ret
rf_notcx:
        cmp     byte ptr [pl_raw],0
        je      rf_vol
        push    edx
        mov     eax,[pl_nstart]
        mov     edx,[pl_nlen]
        call    fd_newtype
        pop     edx
        pop     ebx
        ret
rf_vol:
        cmp     dword ptr [fg_exec+FG_BITS],12
        jne     rf_not12
        mov     al,PT_FAT12
        pop     ebx
        ret
rf_not12:
        cmp     dword ptr [fg_exec+FG_BITS],32
        jne     rf_16
        mov     eax,[pl_nstart]
        add     eax,[pl_nlen]
        cmp     eax,[phy_chs]
        ja      rf_32lba
        mov     al,PT_FAT32
        pop     ebx
        ret
rf_32lba:
        mov     al,PT_FAT32L
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
;   3. say what the plan found, whether or not it is going to run
;   4. run it, unless the writes are held off
;
; STEP THREE HAPPENS EVEN WHEN STEP FOUR WILL NOT.  The screen runs
; every change once with the writes held off, and that run is not a
; refusal, it is a DRY RUN: it walks every check, says what would have
; happened, and cannot touch a sector because dk_write refuses
; unarmed.  THE NUMBERS ARE NOT PRINTED BACK.  The screen draws them
; on the bar and asks about them in its own words, and the command
; line was just given them; a line repeating them in sectors was one
; nobody needed.
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

        call    fd_entptr

;--- the proposal ----------------------------------------------------
        mov     esi,[pl_ent]
        mov     eax,[esi+PE_START]
        add     eax,[pl_base]
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

;--- what the planner said -------------------------------------------
; A REFUSAL BRINGS ITS OWN BLANK LINE (msg_refused), so the one that
; sets the plan off from the command line comes after the test.
        cmp     dword ptr [pl_status],RL_OK
        jne     fdc_refused
        call    crlf

        cmp     byte ptr [pl_crossbl],0
        je      fdc_nowarn
        mov     edx,OFFSET msg_bootwarn
        call    puts
fdc_nowarn:
        cmp     byte ptr [pl_cont],0
        je      fdc_notcx
        mov     edx,OFFSET msg_cxsize   ; a resize moves nothing in it
        mov     eax,[pl_nstart]
        cmp     eax,[pl_ostart]
        je      fdc_say
        mov     edx,OFFSET msg_cxmove   ; and a move moves all of it
        jmp     short fdc_say
fdc_notcx:
        cmp     byte ptr [pl_raw],0
        je      fdc_layout
        mov     edx,OFFSET msg_rawonly
fdc_say:
        call    puts
        jmp     short fdc_armed
fdc_layout:
        call    rm_say                  ; what changing the clusters means,
        mov     edx,OFFSET msg_willbe   ; when they change
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
fdc_armed:
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
        cmp     eax,RL_LAST
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

;=====================================================================
; /C, /D AND /A - CREATE, DELETE, AND SET ACTIVE.
;
; What every other FDISK does, arriving last here because it is the
; easy half and the hard half was worth doing first.  Creating a
; partition is writing sixteen bytes; the work is deciding WHICH
; sixteen, which is where the arithmetic below goes.
;
; ALL THREE GO THROUGH THE SAME SHAPE the move and the resize use:
; work out the proposal, check it, print it whether or not it will
; run, and touch a sector only when the writes are armed - always,
; except in the screen's dry run.  A creation refused for the right
; reason prints as clearly as one that happens.
;
; A NUMBER ABOVE FOUR IS A LOGICAL DRIVE and goes to the chain editor
; further down, which is a different shape of problem: the four slots
; here are four fixed places in one sector, and a logical drive is a
; link in a list written across the space it describes.  /C and /D
; both hand over; /A does not, because booting looks in the master
; boot record and there is no chain in it.
;
; DELETING AN EXTENDED CONTAINER THAT STILL HAS DRIVES IN IT is still
; refused, and now it is a refusal with an answer behind it: delete
; the logical drives first, by number, and the container empties.
;=====================================================================

;---------------------------------------------------------------------
; fd_cylup / fd_cyldn - EAX to the next / previous cylinder boundary.
;                       ECX = sectors per cylinder.  A zero or absurd
;                       geometry leaves the number alone, because an
;                       alignment computed from a geometry nobody
;                       believes is worse than none.
;---------------------------------------------------------------------
fd_cylup:
        push    edx
        or      ecx,ecx
        jz      fcu_out
        push    eax
        add     eax,ecx
        dec     eax
        ; DEC DOES NOT TOUCH CF - which is the quirk this line relies
        ; on rather than works around: the carry being tested is still
        ; the ADD's, and a round-up that ran off the top of thirty-two
        ; bits leaves the number alone instead of wrapping to nearly
        ; nothing and aligning a partition to the start of the disk.
        jc      fcu_pop
        xor     edx,edx
        div     ecx
        mul     ecx
        add     esp,4                   ; the saved EAX, discarded
        pop     edx
        ret
fcu_pop:
        pop     eax
fcu_out:
        pop     edx
        ret

fd_cyldn:
        push    edx
        or      ecx,ecx
        jz      fcd_out
        xor     edx,edx
        div     ecx
        mul     ecx
fcd_out:
        pop     edx
        ret

;---------------------------------------------------------------------
; fd_spcyl - sectors per cylinder of the disk in dsk_rec, in ECX.
;            Zero when the geometry says nothing usable.
;---------------------------------------------------------------------
fd_spcyl:
        push    eax
        movzx   ecx,word ptr [dsk_rec+DSK_SPT]
        movzx   eax,word ptr [dsk_rec+DSK_HEADS]
        imul    ecx,eax
        pop     eax
        ret

;---------------------------------------------------------------------
; fd_aligned - EAX = the first sector of an extent, EDX = one past its
;              last, EBX = the one start that is allowed not to be on
;              a cylinder boundary because something else had to be
;              there first.  CF clear if the extent is aligned.
;
; THE EXCEPTION IS PASSED IN rather than assumed, because there are
; two of them and they are not the same sector.  A primary partition
; may begin one track in, where the master boot record left off; the
; head of an extended chain must begin exactly where the container
; does, wherever that is, because the record the container names is
; the one the walk starts at.  Both are "this sector was not free to
; choose", and neither is a table anyone should be told to fix.
;
; A geometry of zero has no opinion.  It is better to say nothing
; about alignment than to measure it against a cylinder of no sectors.
;---------------------------------------------------------------------
fd_aligned:
        pushad
        call    fd_spcyl                ; ECX = sectors per cylinder
        or      ecx,ecx
        jz      fal_yes
        mov     ebp,edx                 ; EBP = one past the last sector
        mov     esi,eax                 ; ESI = the start, kept for the
        xor     edx,edx                 ; exception below
        div     ecx
        or      edx,edx
        jz      fal_end
        cmp     esi,ebx
        jne     fal_no
fal_end:
        mov     eax,ebp
        xor     edx,edx
        div     ecx
        or      edx,edx
        jnz     fal_no
fal_yes:
        popad
        clc
        ret
fal_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fd_gap - the largest free run of sectors on the disk whose table is
;          in mbr_buf.  [gp_start] and [gp_len] get it; CF set if
;          there is nothing free.
;
; THE FIRST TRACK IS NEVER FREE.  It holds the master boot record, and
; every DOS that ever partitioned a disk started the first partition
; at the beginning of the SECOND track - which is why a 63-sector
; disk's first partition starts at LBA 63 and why anything else, while
; legal, is a disk some other tool will complain about.
;
; The scan is a walk rather than a sort because there are four entries:
; take the lowest occupied start at or above the cursor, the space
; before it is a gap, and the cursor moves past it.
;---------------------------------------------------------------------
fd_gap:
        pushad
        mov     dword ptr [gp_start],0
        mov     dword ptr [gp_len],0
        call    fd_spcyl                ; ECX = sectors per cylinder
        mov     [gp_spcyl],ecx

        movzx   ebx,word ptr [dsk_rec+DSK_SPT]  ; EBX = the cursor
        or      ebx,ebx
        jnz     fg_havefirst
        mov     ebx,1                   ; a geometry with no track length
fg_havefirst:                           ; still may not have the MBR
        mov     ebp,dword ptr [dsk_rec+DSK_TOTAL]

fg_scan:
        cmp     ebx,ebp
        jae     fg_done

        ; the lowest occupied partition starting at or after EBX
        mov     edx,0FFFFFFFFh          ; EDX = its start, none yet
        xor     edi,edi                 ; EDI = its length
        mov     esi,[mbr_buf]
        add     esi,PART_OFF
        xor     eax,eax
fg_one:
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      fg_onenext
        mov     ecx,[esi+PE_START]
        cmp     ecx,ebx                 ; one that ENCLOSES the cursor
        jae     fg_notbehind            ; pushes it past itself
        push    ecx
        add     ecx,[esi+PE_SECS]
        cmp     ecx,ebx
        jbe     fg_popnext
        mov     ebx,ecx                 ; inside it: move on
        pop     ecx
        jmp     short fg_scan
fg_popnext:
        pop     ecx
        jmp     short fg_onenext
fg_notbehind:
        cmp     ecx,edx
        jae     fg_onenext
        mov     edx,ecx
        mov     edi,[esi+PE_SECS]
fg_onenext:
        add     esi,PE_LEN
        inc     eax
        cmp     eax,PART_MAX
        jb      fg_one

        ; the space between the cursor and whatever comes next
        mov     ecx,ebp                 ; nothing ahead: to the end
        cmp     edx,0FFFFFFFFh
        je      fg_measure
        mov     ecx,edx
        cmp     ecx,ebp
        jbe     fg_measure
        mov     ecx,ebp
fg_measure:
        sub     ecx,ebx                 ; ECX = this gap's length
        jbe     fg_step
        ; THE SCREEN NAMES ITS GAP by a sector inside it, because the
        ; free space somebody clicked on is not always the biggest
        ; there is.  The command line never sets gp_want, and gets the
        ; biggest as it always has.
        cmp     dword ptr [gp_want],0
        je      fg_biggest
        mov     eax,[gp_want]
        sub     eax,ebx
        jb      fg_step
        cmp     eax,ecx
        jae     fg_step
        jmp     short fg_take
fg_biggest:
        cmp     ecx,[gp_len]
        jbe     fg_step
fg_take:
        mov     [gp_len],ecx
        mov     [gp_start],ebx
fg_step:
        cmp     edx,0FFFFFFFFh
        je      fg_done
        mov     ebx,edx
        add     ebx,edi                 ; past the partition we found
        jmp     fg_scan

fg_done:
        cmp     dword ptr [gp_len],0
        je      fg_none
        popad
        clc
        ret
fg_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fd_newtype - the type byte a partition of this shape should carry
;              when there is no filesystem in it yet.  EAX = start,
;              EDX = length -> AL.
;
; IT IS A PROMISE ABOUT ADDRESSING AS MUCH AS ABOUT SIZE, which is
; rl_typebyte's argument and applies here for the same reason: 06h
; says "FAT16, reachable by CHS", and a BIOS that meets it on a
; partition ending past the CHS line will read the wrong sector and be
; certain it is right.  So the END is what decides between the CHS
; spellings and the LBA ones.
;
; It differs from rl_typebyte in what it can know.  That one is
; resizing a volume that EXISTS and asks the FAT layout what width it
; came out; this one is describing empty space, so the width is a
; prediction from the size alone - and FORMAT is free to disagree,
; which is why the type may be given explicitly.
;---------------------------------------------------------------------
FAT16_MAXSECS   EQU     4193536         ; 65524 clusters of 64 sectors, the
                                        ; largest volume FAT16 can address

fd_newtype:
        push    ebx
        push    edx
        mov     ebx,eax
        add     ebx,edx                 ; EBX = one past the last sector
        cmp     edx,FAT16_MAXSECS
        ja      fnt_32
        cmp     ebx,[phy_chs]
        ja      fnt_16lba
        cmp     edx,65536               ; 32MB, where 04h stops
        ja      fnt_16big
        mov     al,PT_FAT16S
        jmp     short fnt_out
fnt_16big:
        mov     al,PT_FAT16B
        jmp     short fnt_out
fnt_16lba:
        mov     al,PT_FAT16L
        jmp     short fnt_out
fnt_32:
        mov     al,PT_FAT32
        cmp     ebx,[phy_chs]
        jbe     fnt_out
        mov     al,PT_FAT32L
fnt_out:
        pop     edx
        pop     ebx
        ret

;---------------------------------------------------------------------
; fd_loaddisk - the disk named in pl_drive, and its table.
;               dsk_rec, phy_total, phy_chs and mbr_buf are filled.
;               Jumps to the error labels itself, as fd_change does.
;---------------------------------------------------------------------
fd_loaddisk:
        movzx   edx,byte ptr [pl_drive]
        mov     edi,OFFSET dsk_rec
        mov     eax,0F10Ch
        int     21h
        jc      fdn_nodisk
        test    byte ptr [dsk_rec+DSK_FLAGS],DSKF_PRESENT
        jz      fdn_nodisk
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        mov     [phy_total],eax
        mov     eax,dword ptr [dsk_rec+DSK_CHS]
        mov     [phy_chs],eax

        mov     al,[pl_drive]
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_read
        jc      fdn_unread
        mov     edi,[mbr_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     fdn_notable
        ret
fdn_nodisk:
        jmp     fdc_nodisk
fdn_unread:
        jmp     fdc_unread
fdn_notable:
        jmp     fdc_notable

;---------------------------------------------------------------------
; fd_slotptr - ESI -> the entry pl_idx names, inside mbr_buf.
;---------------------------------------------------------------------
fd_slotptr:
        imul    esi,dword ptr [pl_idx],PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        ret

;--- fd_putnum - the number this partition is called by: 1 to 4 for
;--- a primary slot, 5 and up for a logical drive.  It is the number
;--- the report prints and the number the command line takes back.
fd_putnum:
        push    eax
        mov     eax,[pl_log]
        or      eax,eax
        jnz     fpn_log
        mov     eax,[pl_idx]
        inc     eax
        jmp     short fpn_out
fpn_log:
        add     eax,PART_MAX
fpn_out:
        call    put1
        pop     eax
        ret

;---------------------------------------------------------------------
; fd_writembr - the table back, once.  CF on failure.
;---------------------------------------------------------------------
fd_writembr:
        push    eax
        push    ebx
        push    ecx
        push    edi
        mov     al,[pl_drive]
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_write
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; fd_isext - AL = a type byte.  CF clear if it is an extended
;            container.
;---------------------------------------------------------------------
fd_isext:
        cmp     al,PT_EXTENDED
        je      fie_yes
        cmp     al,PT_EXTENDEDL
        je      fie_yes
        stc
        ret
fie_yes:
        clc
        ret

;=====================================================================
; /C - create a partition.
;
;   FDISK /C d:p        fill the largest free gap
;   FDISK /C d:p n      n sectors of it, or n MB written 40M
;   FDISK /C d:p n tt   ...and tt as the type byte, in hex
;=====================================================================
fd_create:
        cmp     dword ptr [fd_nargs],1
        jb      fdn_usage
        call    fd_getdp
        jc      fdn_usage
        cmp     dword ptr [pl_log],0
        jne     fd_crlog
        call    fd_loaddisk

        ; the slot has to be empty.  Overwriting an occupied one would
        ; be a delete nobody asked for, and the entry it overwrote is
        ; the only record of where that partition was.
        call    fd_slotptr
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        jne     fdn_taken

        call    fd_gap
        jc      fdn_nogap
        call    fd_spcyl
        mov     [gp_spcyl],ecx

        ; ALIGN THE START UP, unless it is already the first usable
        ; sector - which is the one place a partition is allowed to
        ; begin mid-cylinder, because cylinder 0 track 0 belongs to
        ; the master boot record and DOS has started there since 1983.
        mov     eax,[gp_start]
        movzx   ebx,word ptr [dsk_rec+DSK_SPT]
        cmp     eax,ebx
        je      fdn_startok
        mov     ecx,[gp_spcyl]
        call    fd_cylup
fdn_startok:
        mov     [cr_start],eax

        ; how much of the gap was asked for
        mov     edx,[gp_start]
        add     edx,[gp_len]            ; EDX = one past the gap
        sub     edx,[cr_start]
        jbe     fdn_nogap               ; the alignment ate the whole gap
        mov     [cr_len],edx

        cmp     dword ptr [fd_nargs],2
        jb      fdn_havelen
        mov     esi,OFFSET fd_args + FD_ARGLEN
        call    fd_atosz
        jc      fdn_usage
        or      eax,eax
        jz      fdn_usage
        cmp     eax,[cr_len]
        ja      fdn_toobig
        mov     [cr_len],eax
fdn_havelen:

        ; THE END GOES DOWN TO A CYLINDER BOUNDARY, always.  A
        ; partition that ends mid-cylinder is legal and every tool
        ; that ever wrote one has rounded, so a table this program
        ; writes should look like a table anything else would.
        mov     eax,[cr_start]
        add     eax,[cr_len]
        mov     ecx,[gp_spcyl]
        call    fd_cyldn
        sub     eax,[cr_start]
        jbe     fdn_tooshort
        mov     [cr_len],eax

        ; the type: asked for, or worked out from the shape
        cmp     dword ptr [fd_nargs],3
        jb      fdn_autotype
        mov     esi,OFFSET fd_args + FD_ARGLEN*2
        call    fd_atohex
        jc      fdn_usage
        or      al,al
        jz      fdn_usage               ; 00h is "empty", not a type
        mov     [cr_type],al
        jmp     short fdn_havetype
fdn_autotype:
        mov     eax,[cr_start]
        mov     edx,[cr_len]
        call    fd_newtype
        mov     [cr_type],al
fdn_havetype:

;--- say what it would be --------------------------------------------
        call    crlf
        mov     edx,OFFSET msg_crprop
        call    puts
        mov     eax,[pl_idx]
        inc     eax
        call    put1
        mov     edx,OFFSET msg_cron
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_crat
        call    puts
        mov     eax,[cr_start]
        call    put1
        mov     edx,OFFSET msg_plus
        call    puts
        mov     eax,[cr_len]
        call    put1
        mov     edx,OFFSET msg_crsec
        call    puts
        mov     eax,[cr_len]
        call    sec2mb
        call    put1
        mov     edx,OFFSET msg_crmb
        call    puts
        movzx   eax,byte ptr [cr_type]
        call    put2
        call    sp1
        movzx   eax,byte ptr [cr_type]
        call    typename
        call    crlf

        ; past the CHS line is legal and unbootable, and the user
        ; decides - the same report rl_plan makes for a move
        mov     eax,[cr_start]
        add     eax,[cr_len]
        cmp     eax,[phy_chs]
        jbe     fdn_inreach
        mov     edx,OFFSET msg_pastchs
        call    puts
fdn_inreach:

        mov     edx,OFFSET msg_crthen
        call    puts

        cmp     byte ptr [dk_armed],0
        jne     fdn_go
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done

fdn_go:
        call    fd_slotptr
        mov     al,[cr_type]
        mov     [esi+PE_TYPE],al
        mov     byte ptr [esi+PE_BOOT],0
        mov     eax,[cr_start]
        mov     [esi+PE_START],eax
        mov     eax,[cr_len]
        mov     [esi+PE_SECS],eax
        mov     eax,[cr_start]
        lea     edi,[esi+PE_CHSBEG]
        call    lba_to_chs
        mov     eax,[cr_start]
        add     eax,[cr_len]
        dec     eax
        lea     edi,[esi+PE_CHSEND]
        call    lba_to_chs
        call    fd_writembr
        jc      fdn_wrerr
        mov     edx,OFFSET msg_crdone
        call    puts
        jmp     fd_done

;=====================================================================
; /D - delete one.
;=====================================================================
fd_delete:
        cmp     dword ptr [fd_nargs],1
        jb      fdn_usage
        call    fd_getdp
        jc      fdn_usage
        cmp     dword ptr [pl_log],0
        jne     fd_dellog
        call    fd_loaddisk
        call    fd_slotptr
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      fdn_alreadyempty

        ; AN EXTENDED CONTAINER WITH ANYTHING IN IT IS REFUSED, because
        ; deleting it would orphan every logical drive inside it -
        ; their EBRs would still be on the disk, chained to each other,
        ; described by nothing.  Emptying the chain first is the right
        ; order, and /D on a logical drive does that one at a time -
        ; so the refusal is now a refusal with an answer behind it.
        mov     al,[esi+PE_TYPE]
        call    fd_isext
        jc      fdn_notext
        push    esi
        mov     eax,[esi+PE_START]
        call    fd_chainbusy
        pop     esi
        jc      fdn_chainbusy
fdn_notext:

        call    crlf
        mov     edx,OFFSET msg_delprop
        call    puts
        mov     eax,[pl_idx]
        inc     eax
        call    put1
        mov     edx,OFFSET msg_cron
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_crat
        call    puts
        mov     eax,[esi+PE_START]
        call    put1
        mov     edx,OFFSET msg_plus
        call    puts
        mov     eax,[esi+PE_SECS]
        call    put1
        mov     edx,OFFSET msg_crsec
        call    puts
        mov     edx,OFFSET msg_deltype
        call    puts
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    put2
        call    sp1
        movzx   eax,byte ptr [esi+PE_TYPE]
        call    typename
        call    crlf

        ; If the kernel gave it a letter, that letter is about to
        ; describe nothing until the machine restarts.  vol_letter
        ; wants the disk INDEX in EBX and the volume table loaded -
        ; fd_report does both at its start and this verb has to do
        ; them for itself.
        push    esi
        call    vol_load
        movzx   ebx,byte ptr [pl_drive]
        sub     ebx,80h
        pop     esi
        mov     eax,[esi+PE_START]
        push    esi
        call    vol_letter
        pop     esi
        jc      fdn_noletter
        push    esi
        push    eax
        mov     edx,OFFSET msg_delltr
        call    puts
        pop     eax
        call    putc
        mov     al,':'
        call    putc
        mov     edx,OFFSET msg_delltr2
        call    puts
        pop     esi
fdn_noletter:
        mov     edx,OFFSET msg_delgone
        call    puts

        cmp     byte ptr [dk_armed],0
        jne     fdn_delgo
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done
fdn_delgo:
        ; sixteen bytes of zero.  The WHOLE entry and not just the
        ; type byte: a slot with a type of zero and a start still in
        ; it reads as empty to every tool and as occupied to anything
        ; that looks at the numbers, which is the worst of both.
        push    edi
        mov     edi,esi
        xor     eax,eax
        mov     ecx,PE_LEN / 4
        cld
        rep     stosd
        pop     edi
        call    fd_writembr
        jc      fdn_wrerr
        mov     edx,OFFSET msg_deldone
        call    puts
        jmp     fd_done

;=====================================================================
; /A - make one active, which is to say bootable.
;
; ONE AT A TIME, which is not a rule of the format but is a rule of
; every boot sector ever written: MBRBOOT.BIN, and IBM's, and every
; other one, scan the four slots and take the first with 80h in it.
; A table with two active partitions boots the lower-numbered one and
; silently ignores the intent of whoever set the other.
;=====================================================================
fd_active:
        cmp     dword ptr [fd_nargs],1
        jb      fdn_usage
        call    fd_getdp
        jc      fdn_usage
        cmp     dword ptr [pl_log],0
        jne     fdn_logactive           ; booting looks in the master boot
                                        ; record, and never finds a chain
        call    fd_loaddisk
        call    fd_slotptr
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      fdn_alreadyempty

        call    crlf
        mov     edx,OFFSET msg_actprop
        call    puts
        mov     eax,[pl_idx]
        inc     eax
        call    put1
        mov     edx,OFFSET msg_cron
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_actrest
        call    puts

        mov     eax,[esi+PE_START]
        cmp     eax,[phy_chs]
        jb      fdn_actreach
        mov     edx,OFFSET msg_pastchs
        call    puts
fdn_actreach:

        cmp     byte ptr [dk_armed],0
        jne     fdn_actgo
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done
fdn_actgo:
        mov     esi,[mbr_buf]
        add     esi,PART_OFF
        xor     ecx,ecx
fdn_actclr:
        mov     byte ptr [esi+PE_BOOT],0
        add     esi,PE_LEN
        inc     ecx
        cmp     ecx,PART_MAX
        jb      fdn_actclr
        call    fd_slotptr
        mov     byte ptr [esi+PE_BOOT],80h
        call    fd_writembr
        jc      fdn_wrerr
        mov     edx,OFFSET msg_actdone
        call    puts
        jmp     fd_done


;=====================================================================
; /MBR - REPLACE THE BOOT CODE AND NOTHING ELSE.
;
;   FDISK /MBR d      write the boot code of disk d
;
; What it is FOR: a master boot record whose code has been replaced by
; something that no longer works - a boot manager that was uninstalled
; badly, or a virus of the sort that used to live in exactly these
; bytes - leaves a disk whose partitions are all perfectly intact and
; which will not start.  The table is the valuable part and it is
; still there; only the loader in front of it needs putting back.
;
; FOUR HUNDRED AND FORTY BYTES, NOT FOUR HUNDRED AND FORTY-SIX.  The
; boot code proper ends at 1B7h; the six bytes after it are where
; every system since Windows NT has written the disk's own identity,
; and the partition table starts at 1BEh.  DOS knew nothing about
; those six bytes and its FDISK wrote straight over them, which on a
; modern machine means every drive letter is reassigned on the next
; boot.  This leaves them alone.  Our loader is 229 bytes, so there
; is no argument about the room.
;
; IT INSISTS ON A SIGNATURE, which looks like the wrong way round -
; surely a damaged record is the case this is for.  But what makes
; the operation safe is that the partition table SURVIVES it, and a
; sector with no AA55 in it is a sector this program cannot say that
; about.  Writing a loader in front of sixty-four bytes of unknown
; means building a disk that looks bootable and is not.
;=====================================================================

fd_mbr:
        cmp     dword ptr [fd_nargs],1
        jb      fdn_usage
        call    fd_getd
        jc      fdn_usage
        call    fd_loaddisk

        call    crlf
        mov     edx,OFFSET msg_mbrprop
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_mbrhow
        call    puts

        ; THE TABLE THAT STAYS, printed.  It is the thing being
        ; preserved, so it is the thing worth looking at before
        ; agreeing to the write - and it is one call, because the
        ; report already knows how to print it.
        call    vol_load
        movzx   ebx,byte ptr [pl_drive]
        sub     ebx,80h
        call    fr_table

        mov     esi,OFFSET mbr_code
        mov     edi,[mbr_buf]
        mov     ecx,MBRW_LEN
        cld
        repe    cmpsb
        jne     fmb_differs
        mov     edx,OFFSET msg_mbrsame
        call    puts
        jmp     fd_done
fmb_differs:
        mov     edx,OFFSET msg_mbrwarn
        call    puts

        cmp     byte ptr [dk_armed],0
        jne     fmb_go
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done
fmb_go:
        mov     esi,OFFSET mbr_code
        mov     edi,[mbr_buf]
        mov     ecx,MBRW_LEN
        cld
        rep     movsb
        call    fd_writembr
        jc      fdn_wrerr
        mov     edx,OFFSET msg_mbrdone
        call    puts
        jmp     fd_done

;=====================================================================
; FORMAT - a new, empty FAT volume in a partition.  THE SCREEN'S ONLY:
; there is no switch for it, and the command line has FORMAT.  It is a
; verb all the same, run the way the screen runs every other - once
; with the writes held off to find out what it would make, and once
; for real - with fd_args saying d:p.
;
; A QUICK FORMAT: a boot sector, the FATs and an empty root directory,
; laid out for the partition's size, and the data area left as it is.
; The boot sector boots nothing and says so; SYS puts a system on it.
;
; THE LAYOUT IS CHOSEN, NOT KEPT, which is the difference from FORMAT:
; that reformats the BPB a volume already has, and a partition just
; made has none.  The width goes by the size - FAT32 past what FAT16
; can address, or where the type byte already says FAT32 and the size
; can carry it; FAT12 for 4MB and less, where FAT16 cannot have enough
; clusters; FAT16 everywhere else - and the cluster size is Microsoft's
; for that size and width, so the volume is the one DOS and Windows
; would have made.  The type byte is then made to say what it is.
;=====================================================================
FF12_MAX        EQU     8400            ; FAT12 at and below this many sectors
FF32_MIN        EQU     66600           ; and FAT32 not below this many
FF_ROOTENT      EQU     512             ; FAT12 and FAT16's root directory
FF_RSVD32       EQU     32              ; FAT32's reserved sectors, with the
FF_FSINFO       EQU     1               ; FSInfo sector in the second and the
FF_BKBOOT       EQU     6               ; spare boot sector in the seventh
FF_MEDIA        EQU     0F8h            ; a fixed disk

fd_format:
        cmp     dword ptr [fd_nargs],1
        jb      fdn_usage
        call    fd_getdp
        jc      fdn_usage
        call    fd_loaddisk
        call    fd_entptr
        mov     esi,[pl_ent]
        mov     al,[esi+PE_TYPE]
        cmp     al,PT_EMPTY
        je      ffm_none
        call    fd_isext
        jnc     ffm_cont
        mov     eax,[esi+PE_START]
        add     eax,[pl_base]
        mov     [ff_start],eax
        mov     eax,[esi+PE_SECS]
        mov     [ff_len],eax
        mov     dl,[esi+PE_TYPE]
        call    ff_layout
        jc      ffm_small

        mov     edx,OFFSET msg_fmtwill
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

        cmp     byte ptr [dk_armed],0
        jne     ffm_go
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done
ffm_go:
        mov     edx,OFFSET msg_fmtdoing
        call    puts
        call    ff_write
        jc      fdn_wrerr
        ; and the type byte, to say what the partition now holds
        mov     byte ptr [pl_cont],0
        mov     byte ptr [pl_raw],0
        mov     eax,[ff_start]
        mov     [pl_nstart],eax
        mov     eax,[ff_len]
        mov     [pl_nlen],eax
        call    rl_typebyte
        mov     esi,[pl_ent]
        cmp     al,[esi+PE_TYPE]
        je      ffm_typed
        mov     [esi+PE_TYPE],al
        mov     edx,OFFSET msg_wrtab
        call    puts
        mov     edi,[mbr_buf]
        cmp     dword ptr [pl_sec],0
        je      ffm_wrsec
        mov     edi,[ebr_buf]
ffm_wrsec:
        mov     al,[pl_drive]
        mov     ebx,[pl_sec]
        mov     ecx,1
        call    dk_write
        jc      fdn_wrerr
ffm_typed:
        mov     edx,OFFSET msg_fmtdone
        call    puts
        jmp     fd_done
ffm_none:
        mov     edx,OFFSET msg_fmtnone
        jmp     short ffm_say
ffm_cont:
        mov     edx,OFFSET msg_fmtcont
        jmp     short ffm_say
ffm_small:
        mov     edx,OFFSET msg_fmtsmall
ffm_say:
        call    puts
        jmp     fd_fail

;---------------------------------------------------------------------
; ff_layout - the volume for [ff_len] sectors.  DL = the type byte the
;             partition has now.  -> fg_exec, and fv_rsvd, fv_rootent,
;             fv_nfats and fv_bps as the layout code reads them.  CF set
;             when no FAT volume fits.
;
; fg_forspc does the arithmetic, as it does for a resize; here the BPB
; choices it takes from the volume are this program's own.  A cluster
; size that comes out the wrong width is walked a step: halved when
; there are too few clusters, doubled when there are too many.
;---------------------------------------------------------------------
ff_layout:
        pushad
        mov     dword ptr [ff_iter],16  ; walking the cluster sizes is
                                        ; bounded, as fg_settle is
        mov     dword ptr [fv_bps],SEC_BYTES
        mov     dword ptr [fv_nfats],2
        mov     eax,[ff_len]
        cmp     eax,FAT16_MAXSECS
        ja      ffl_32
        cmp     eax,FF32_MIN
        jb      ffl_small
        cmp     dl,PT_FAT32
        je      ffl_32
        cmp     dl,PT_FAT32L
        je      ffl_32
ffl_small:
        mov     dword ptr [fv_rsvd],1
        mov     dword ptr [fv_rootent],FF_ROOTENT
        cmp     dword ptr [ff_len],FF12_MAX
        jbe     ffl_12
        mov     esi,OFFSET ff_tab16
        call    ff_spcfor               ; -> ECX
ffl_16try:
        call    ff_try
        jc      ffl_16up
        cmp     dword ptr [fg_exec+FG_BITS],16
        je      ffl_ok
        cmp     dword ptr [fg_exec+FG_BITS],12
        jne     ffl_16up
        shr     ecx,1                   ; too few clusters: smaller ones
        jz      ffl_no
        jmp     short ffl_16try
ffl_16up:
        shl     ecx,1                   ; too many: larger ones
        cmp     ecx,RM_SPCMAX
        jbe     ffl_16try
        jmp     short ffl_no

ffl_12:
        mov     ecx,1                   ; the smallest cluster FAT12 can count
ffl_12try:
        call    ff_try
        jc      ffl_12up
        cmp     dword ptr [fg_exec+FG_BITS],12
        je      ffl_ok
ffl_12up:
        shl     ecx,1
        cmp     ecx,RM_SPCMAX
        jbe     ffl_12try
        jmp     short ffl_no

ffl_32:
        mov     dword ptr [fv_rsvd],FF_RSVD32
        mov     dword ptr [fv_rootent],0
        mov     esi,OFFSET ff_tab32
        call    ff_spcfor
ffl_32try:
        call    ff_try
        jc      ffl_32down
        cmp     dword ptr [fg_exec+FG_BITS],32
        je      ffl_ok
ffl_32down:
        shr     ecx,1                   ; too few clusters for FAT32
        jnz     ffl_32try
        cmp     dword ptr [ff_len],FAT16_MAXSECS
        jbe     ffl_small               ; FAT16 can have it after all
ffl_no:
        popad
        stc
        ret
ffl_ok:
        popad
        clc
        ret

;--- ff_try - fg_exec for [ff_len] sectors in ECX-sector clusters.  CF
;--- when they make no volume, and once the walk has gone on too long
ff_try:
        dec     dword ptr [ff_iter]
        jz      fft_no
        push    eax
        push    edi
        mov     eax,[ff_len]
        mov     edi,OFFSET fg_exec
        call    fg_forspc
        pop     edi
        pop     eax
        ret
fft_no:
        inc     dword ptr [ff_iter]     ; stays at one: every try now fails
        stc
        ret

;--- ff_spcfor - ESI -> a table of (up to this many sectors, this cluster
;--- size) -> ECX = the cluster size for [ff_len]
ff_spcfor:
        push    eax
        push    esi
        mov     eax,[ff_len]
ffs_row:
        mov     ecx,[esi+4]
        cmp     eax,[esi]
        jbe     ffs_out
        add     esi,8
        jmp     short ffs_row           ; the last row's limit is every size
ffs_out:
        pop     esi
        pop     eax
        ret

;---------------------------------------------------------------------
; ff_write - the volume in fg_exec, at [ff_start].  CF on failure.
;
; Zeros first - the reserved area, every FAT and the root directory,
; and FAT32's root cluster - so that nothing of what the partition held
; is left where the new volume looks; then the boot sector, FAT32's
; FSInfo sector and the spare copies of both; then the first sector of
; each FAT, which is where the media byte and the end markers go.
;---------------------------------------------------------------------
ff_write:
        pushad
        mov     edi,[cpy_buf]
        xor     eax,eax
        mov     ecx,65536 / 4
        rep     stosd
        mov     eax,dword ptr [fg_exec+FG_SPF]
        imul    eax,[fv_nfats]
        add     eax,[fv_rsvd]
        add     eax,dword ptr [fg_exec+FG_ROOT]
        mov     [ff_fatend],eax         ; where the data area starts
        mov     ecx,eax
        mov     ebx,[ff_start]
        call    ff_zero
        jc      ffw_bad
        cmp     dword ptr [fg_exec+FG_BITS],32
        jne     ffw_boot
        mov     ebx,[ff_start]          ; FAT32's root: cluster 2, the
        add     ebx,[ff_fatend]         ; first in the data area
        mov     ecx,dword ptr [fg_exec+FG_SPC]
        call    ff_zero
        jc      ffw_bad

ffw_boot:
        call    ff_boot                 ; -> sec_buf
        mov     ebx,[ff_start]
        call    ff_wrsec
        jc      ffw_bad
        cmp     dword ptr [fg_exec+FG_BITS],32
        jne     ffw_fats
        mov     ebx,[ff_start]
        add     ebx,FF_BKBOOT
        call    ff_wrsec
        jc      ffw_bad
        call    ff_mkfsi                ; -> sec_buf
        mov     ebx,[ff_start]
        add     ebx,FF_FSINFO
        call    ff_wrsec
        jc      ffw_bad
        mov     ebx,[ff_start]
        add     ebx,FF_BKBOOT + FF_FSINFO
        call    ff_wrsec
        jc      ffw_bad

ffw_fats:
        mov     edi,[sec_buf]
        push    edi
        xor     eax,eax
        mov     ecx,SEC_BYTES / 4
        rep     stosd
        pop     edi
        mov     eax,0FFFFFF00h OR FF_MEDIA
        cmp     dword ptr [fg_exec+FG_BITS],32
        jne     ffw_narrow
        mov     eax,0FFFFFF8h           ; entry 0: the media byte, entry 1
        mov     [edi],eax               ; the end marker, and entry 2 the
        mov     dword ptr [edi+4],0FFFFFFFh     ; root directory's one
        mov     dword ptr [edi+8],0FFFFFFFh     ; cluster
        jmp     short ffw_copies
ffw_narrow:
        mov     [edi],ax                ; F8 FF, and FF FF for FAT16 or
        mov     byte ptr [edi+2],0FFh   ; FF for FAT12's twelve bits
        cmp     dword ptr [fg_exec+FG_BITS],16
        jne     ffw_copies
        mov     byte ptr [edi+3],0FFh
ffw_copies:
        mov     ebx,[ff_start]
        add     ebx,[fv_rsvd]
        mov     ebp,[fv_nfats]
ffw_copy:
        call    ff_wrsec
        jc      ffw_bad
        add     ebx,dword ptr [fg_exec+FG_SPF]
        dec     ebp
        jnz     ffw_copy
        popad
        clc
        ret
ffw_bad:
        popad
        stc
        ret

;--- ff_wrsec - sec_buf to sector EBX of the disk ---------------------
ff_wrsec:
        push    edi
        mov     edi,[sec_buf]
        call    fd_secwr
        pop     edi
        ret

;--- ff_zero - ECX sectors from EBX, zeros; cpy_buf holds them --------
ff_zero:
        pushad
ffz_one:
        or      ecx,ecx
        jz      ffz_ok
        mov     edx,ecx
        cmp     edx,65536 / SEC_BYTES
        jbe     ffz_have
        mov     edx,65536 / SEC_BYTES
ffz_have:
        push    ecx
        mov     al,[pl_drive]
        mov     ecx,edx
        mov     edi,[cpy_buf]
        call    dk_write
        pop     ecx
        jc      ffz_bad
        add     ebx,edx
        sub     ecx,edx
        jmp     short ffz_one
ffz_ok:
        popad
        clc
        ret
ffz_bad:
        popad
        stc
        ret

;--- what a formatted volume is made of ------------------------------
; THE BOOT CODE OF A VOLUME WITH NO SYSTEM ON IT, which is FORMAT's for
; a diskette (boot_stub there) with a hard disk's message.  Sixteen-bit
; code, so it is bytes: a stack, the message through INT 10h, a key,
; and INT 19h to start again.  It is position-independent except for
; the one address it loads into SI, which ff_boot fills in - the code
; sits at 3Eh on FAT12 and FAT16 and at 5Ah on FAT32.
ff_code     db  0FAh                    ; cli
            db  31h, 0C0h               ; xor ax,ax
            db  8Eh, 0D0h               ; mov ss,ax
            db  0BCh, 00h, 7Ch          ; mov sp,7C00h
            db  0FBh                    ; sti
            db  8Eh, 0D8h               ; mov ds,ax
FF_SIOFF    EQU $ - ff_code + 1
            db  0BEh, 0, 0              ; mov si,the message
            db  0FCh                    ; cld
            db  0ACh                    ; lodsb
            db  08h, 0C0h               ; or al,al
            db  74h, 09h                ; jz the key
            db  0B4h, 0Eh               ; mov ah,0Eh
            db  0BBh, 07h, 00h          ; mov bx,7
            db  0CDh, 10h               ; int 10h
            db  0EBh, 0F2h              ; jmp lodsb
            db  30h, 0E4h               ; xor ah,ah
            db  0CDh, 16h               ; int 16h
            db  0CDh, 19h               ; int 19h
FF_MSGOFF   EQU $ - ff_code
            db  'No system on this volume.',13,10
            db  'Press a key to restart.',13,10,0
FF_CODELEN  EQU $ - ff_code
ff_oem      db  'PMDOS0.6'              ; as FORMAT names itself
ff_label    db  'NO NAME    '
ff_name12   db  'FAT12   '
ff_name16   db  'FAT16   '
ff_name32   db  'FAT32   '
; Microsoft's cluster sizes: up to this many sectors, this many to a
; cluster.  The last row of each takes every size there is.
ff_tab16    dd  32680, 2, 262144, 4, 524288, 8, 1048576, 16
            dd  2097152, 32, 0FFFFFFFFh, 64
ff_tab32    dd  532480, 1, 16777216, 8, 33554432, 16, 67108864, 32
            dd  0FFFFFFFFh, 64

;---------------------------------------------------------------------
; ff_boot - the boot sector for fg_exec, in sec_buf.
;
; FAT32's BPB runs on to 52h and the others' stops at 24h, so the same
; extended fields - drive, signature, serial, label, the width's name -
; sit 1Ch further on, and so does the code: the jump says where.
;---------------------------------------------------------------------
ff_boot:
        pushad
        mov     edi,[sec_buf]
        push    edi
        xor     eax,eax
        mov     ecx,SEC_BYTES / 4
        rep     stosd
        pop     edi
        mov     byte ptr [edi],0EBh
        mov     byte ptr [edi+2],90h
        mov     esi,OFFSET ff_oem
        push    edi
        add     edi,3
        mov     ecx,8
        rep     movsb
        pop     edi
        mov     word ptr [edi+BPB_BPS],SEC_BYTES
        mov     eax,dword ptr [fg_exec+FG_SPC]
        mov     [edi+BPB_SPC],al
        mov     eax,[fv_rsvd]
        mov     [edi+BPB_RSVD],ax
        mov     eax,[fv_nfats]
        mov     [edi+BPB_NFATS],al
        mov     eax,[fv_rootent]
        mov     [edi+BPB_ROOTENT],ax
        mov     byte ptr [edi+BPB_MEDIA],FF_MEDIA
        movzx   eax,word ptr [dsk_rec+DSK_SPT]
        mov     [edi+BPB_SECPERTRK],ax
        movzx   eax,word ptr [dsk_rec+DSK_HEADS]
        mov     [edi+BPB_NUMHEADS],ax
        mov     eax,[ff_start]
        mov     [edi+BPB_HIDDSEC],eax
        mov     eax,[ff_len]
        cmp     dword ptr [fg_exec+FG_BITS],32
        je      ffb_tot32
        cmp     eax,10000h
        jae     ffb_tot32
        mov     [edi+BPB_TOTSEC16],ax
        jmp     short ffb_spf
ffb_tot32:
        mov     [edi+BPB_TOTSEC32],eax
ffb_spf:
        mov     eax,dword ptr [fg_exec+FG_SPF]
        mov     ebx,24h                 ; EBX = where the extended fields go
        mov     edx,OFFSET ff_name16
        cmp     dword ptr [fg_exec+FG_BITS],32
        je      ffb_32
        mov     [edi+BPB_SPF16],ax
        cmp     dword ptr [fg_exec+FG_BITS],16
        je      ffb_ext
        mov     edx,OFFSET ff_name12
        jmp     short ffb_ext
ffb_32:
        mov     [edi+BPB_SPF32],eax
        mov     dword ptr [edi+BPB_ROOTCLUS],2
        mov     word ptr [edi+BPB_FSINFO],FF_FSINFO
        mov     word ptr [edi+BPB_BKBOOT],FF_BKBOOT
        mov     ebx,40h
        mov     edx,OFFSET ff_name32
ffb_ext:
        lea     eax,[ebx+1Ah-2]         ; JMP SHORT to the code, which
        mov     [edi+1],al              ; follows the extended fields
        add     ebx,edi
        mov     byte ptr [ebx],80h      ; the drive
        mov     byte ptr [ebx+2],29h    ; the signature that says the
        call    ff_serial               ; next three are there
        mov     [ebx+3],eax
        mov     esi,OFFSET ff_label
        push    edi
        lea     edi,[ebx+7]
        mov     ecx,11
        rep     movsb
        mov     esi,edx                 ; the width's name
        mov     ecx,8
        rep     movsb
        mov     esi,OFFSET ff_code      ; and the code, at 3Eh or 5Ah
        mov     ecx,FF_CODELEN
        rep     movsb
        pop     edi
        ; the code says where its message is: 7C00h, where the BIOS puts
        ; the sector, plus wherever the code landed
        lea     eax,[ebx+1Ah+FF_MSGOFF]
        sub     eax,edi
        add     eax,7C00h
        mov     [ebx+1Ah+FF_SIOFF],ax
        mov     word ptr [edi+1FEh],MBR_SIG
        popad
        ret

;--- ff_serial - EAX = a volume serial number, out of the clock: the
;--- time and the date added, as FORMAT makes them
ff_serial:
        push    ebx
        push    ecx
        push    edx
        mov     ah,2Ch
        int     21h
        mov     ebx,ecx
        shl     ebx,16
        mov     bx,dx
        mov     ah,2Ah
        int     21h
        shl     ecx,16
        mov     cx,dx
        lea     eax,[ebx+ecx]
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- ff_mkfsi - FAT32's FSInfo sector, in sec_buf: every cluster free
;--- but the root's, and the next to give out the one after it
ff_mkfsi:
        pushad
        mov     edi,[sec_buf]
        push    edi
        xor     eax,eax
        mov     ecx,SEC_BYTES / 4
        rep     stosd
        pop     edi
        mov     dword ptr [edi],FSI_SIG1
        mov     dword ptr [edi+1E4h],FSI_SIG2
        mov     eax,dword ptr [fg_exec+FG_CLUS]
        dec     eax
        mov     [edi+FSI_FREE],eax
        mov     dword ptr [edi+FSI_NEXT],3
        mov     dword ptr [edi+1FCh],0AA550000h
        popad
        ret

;---------------------------------------------------------------------
; fd_getd - the first argument as a bare disk number.
;
; /MBR is the one verb that names a DISK and not a partition, because
; the master boot record is not in any of them.  d:p would have to
; mean "the disk that partition is on", which is a longer way of
; saying the same thing and invites somebody to believe the partition
; mattered.
;---------------------------------------------------------------------
fd_getd:
        push    eax
        push    esi
        mov     esi,OFFSET fd_args
        call    fd_atoi
        jc      fgt_bad
        cmp     eax,MAX_DISKS
        jae     fgt_bad
        add     eax,80h
        mov     [pl_drive],al
        cmp     byte ptr [esi],0
        jne     fgt_bad
        pop     esi
        pop     eax
        clc
        ret
fgt_bad:
        pop     esi
        pop     eax
        stc
        ret
;---------------------------------------------------------------------
; fd_chainbusy - EAX = the first EBR of an extended container.
;                CF set if anything is chained inside it.
;
; ONE SECTOR IS ENOUGH TO ANSWER IT: an empty container has an EBR
; whose slots are all empty, or no readable EBR at all.  A container
; that cannot be read is treated as busy, because "I could not tell"
; and "it is empty" must not come back as the same answer when the
; consequence is orphaning somebody's volumes.
;---------------------------------------------------------------------
fd_chainbusy:
        pushad
        mov     ebx,eax
        mov     al,[pl_drive]
        mov     ecx,1
        mov     edi,[ebr_buf]
        call    dk_read
        jc      fcb_busy                ; unreadable: assume the worst
        mov     esi,[ebr_buf]
        cmp     word ptr [esi+1FEh],MBR_SIG
        jne     fcb_free                ; no record there at all
        add     esi,PART_OFF
        xor     ecx,ecx
fcb_one:
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        jne     fcb_busy
        add     esi,PE_LEN
        inc     ecx
        cmp     ecx,PART_MAX
        jb      fcb_one
fcb_free:
        popad
        clc
        ret
fcb_busy:
        popad
        stc
        ret


;=====================================================================
; THE CHAIN, EDITED.
;
; What the previous release said it would not do, and said why: a
; logical drive is not a table entry, it is a LINK - a boot record of
; its own, a volume one track behind it, and a pointer to it in the
; record before.  Adding or removing one is surgery on a singly
; linked list that is written across the very space it describes.
;
; THE HEAD OF THE LIST CANNOT MOVE, and that is the whole reason this
; is harder than it sounds.  The master boot record names the sector
; the chain starts at, and every slot-1 offset in every record of the
; chain is measured from that same sector.  Move the head and every
; link in the list needs rewriting; so the head never moves, and the
; two operations below are arranged around that.  (A move of the whole
; container moves the head too, and every link with it by the same
; amount - which is the one way of moving it that leaves every offset
; as it was.  That is rl_cxmove.)
;
; NOTHING HERE TOUCHES THE MASTER BOOT RECORD.  The container does not
; change size or position when something inside it comes or goes, and
; a program that adjusted it anyway would be answering a question
; nobody asked with a write to the one sector on the disk that has to
; be right.
;
; ONE SECTOR IS WRITTEN BY A DELETE AND TWO BY A CREATE, and where
; there are two the ORDER is the argument: the new record goes down
; first, while it is still a sector nothing refers to, and the pointer
; to it second.  Lose power between them and the chain is exactly what
; it was.  The other order puts a live pointer on the disk to a record
; that has not been written yet, which is a chain that walks into
; somebody's data.
;=====================================================================

;---------------------------------------------------------------------
; fd_loadchain - the disk, its table and its chain, or a refusal.
;---------------------------------------------------------------------
fd_loadchain:
        call    fd_loaddisk
        call    fd_chain
        jc      fdn_nocont
        cmp     dword ptr [chn_st],CHN_OK
        jne     fdn_chbad
        ret

;---------------------------------------------------------------------
; fd_secrd / fd_secwr - one sector of pl_drive.  EBX = its number,
;                       EDI -> the buffer.  CF on failure.
;---------------------------------------------------------------------
fd_secrd:
        push    eax
        push    ecx
        mov     al,[pl_drive]
        mov     ecx,1
        call    dk_read
        pop     ecx
        pop     eax
        ret
fd_secwr:
        push    eax
        push    ecx
        mov     al,[pl_drive]
        mov     ecx,1
        call    dk_write
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; fd_putent - one partition entry, written properly.
;             EDI -> the entry, AL = type, EBX = the absolute first
;             sector, ECX = the length, EDX = what this entry's start
;             field is measured from.
;
; THE BASE IS AN ARGUMENT because in an extended boot record it is two
; different things one entry apart, and a routine that assumed either
; one would be wrong half the time.  Slot 0 is measured from the
; record it is written in; slot 1 from the first record of the chain.
;---------------------------------------------------------------------
fd_putent:
        pushad
        mov     [edi+PE_TYPE],al
        ; PE_BOOT IS NOT TOUCHED.  A partition that was the active one
        ; is still the active one after it has been moved or resized,
        ; and the callers that are building a FRESH entry are already
        ; writing into a sector they cleared, or say so themselves.
        mov     eax,ebx
        sub     eax,edx
        mov     [edi+PE_START],eax
        mov     [edi+PE_SECS],ecx
        mov     eax,ebx
        push    edi
        add     edi,PE_CHSBEG
        call    lba_to_chs
        pop     edi
        mov     eax,ebx
        add     eax,ecx
        dec     eax                     ; the LAST sector, not one past it
        add     edi,PE_CHSEND
        call    lba_to_chs
        popad
        ret


;=====================================================================
; fd_entptr - THE ENTRY THIS RUN IS ABOUT, wherever it lives.
;
;   pl_ent   -> its sixteen bytes, in mbr_buf or in ebr_buf
;   pl_base  what its start field is measured from: 0 for a primary
;            partition, the record's own sector for a logical drive
;   pl_sec   the sector that has to be written back afterwards
;   pl_lo    the region it is not allowed to leave - the disk for a
;   pl_hi    primary, the container for a logical drive
;   nb_tbl   and every wall inside that region
;
; ONE POINTER AND ONE BASE is the whole difference between a primary
; partition and a logical drive, as far as the planner and the
; executor are concerned.  Everything else they do - reading the BPB,
; scanning the FAT for the highest cluster in use, moving three
; regions in an order chosen by the direction, laying down a new boot
; record - is about a VOLUME AT A SECTOR, and a volume has no idea
; which kind of table entry describes it.
;
; That is why /M and /S on a logical drive cost a resolver and two
; wall-builders rather than a second copy of the engine.  The engine
; was already right; it was being handed its argument in only one of
; the two ways a partition table can express it.
;
; The disk and mbr_buf must already be loaded.  Refusals are taken
; here, in the same shape the chain verbs use.
;=====================================================================
fd_entptr:
        cmp     dword ptr [pl_log],0
        jne     fep_log

        call    fd_slotptr
        mov     [pl_ent],esi
        mov     dword ptr [pl_base],0
        mov     dword ptr [pl_sec],0
        ; SECTOR ZERO IS THE MASTER BOOT RECORD and belongs to nobody,
        ; so the region starts at one and not at zero.
        mov     dword ptr [pl_lo],1
        mov     eax,[phy_total]
        mov     [pl_hi],eax
        call    fd_nbprim
        ret

fep_log:
        call    fd_chain
        jc      fdn_nocont
        cmp     dword ptr [chn_st],CHN_OK
        jne     fdn_chbad
        mov     ebp,[pl_log]
        dec     ebp
        cmp     ebp,[chn_n]
        jae     fdn_nolog
        mov     [lg_idx],ebp
        imul    esi,ebp,CH_LEN
        add     esi,[chn_tbl]
        cmp     dword ptr [esi+CH_VSTART],0
        je      fdn_nolog

        mov     eax,[esi+CH_EBR]
        mov     [pl_sec],eax
        mov     [pl_base],eax           ; slot 0 is measured from its own
        mov     ebx,eax                 ; record, and only from that
        mov     edi,[ebr_buf]
        call    fd_secrd
        jc      fdn_rderr
        mov     eax,[ebr_buf]
        add     eax,PART_OFF
        mov     [pl_ent],eax
        ; NOT THE CONTAINER'S START BUT ITS OWN RECORD'S.  The volume's
        ; start is written as an offset from that record, and an offset
        ; has no sign: a volume moved in front of the record that names
        ; it - into space a deleted drive left, say, which the walls
        ; alone would allow - would be written as a start four
        ; gigabytes on.  So it may go anywhere after its record and up
        ; to the container's end.
        mov     eax,[pl_sec]
        inc     eax
        mov     [pl_lo],eax
        mov     eax,[chn_base]
        add     eax,[chn_ext]
        mov     [pl_hi],eax
        call    fd_nblog
        ret

;---------------------------------------------------------------------
; fd_nbprim - the walls around a primary partition: the other three
;             slots, whichever of them are occupied.
;
; The extended container counts as one of them, which is how a primary
; resize is stopped from growing into somebody's logical drives: the
; container's own entry covers every one of them.
;---------------------------------------------------------------------
fd_nbprim:
        pushad
        mov     dword ptr [nb_n],0
        mov     edi,OFFSET nb_tbl
        xor     ebp,ebp
nbp_one:
        cmp     ebp,PART_MAX
        jae     nbp_done
        cmp     ebp,[pl_idx]
        je      nbp_step
        imul    esi,ebp,PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      nbp_step
        mov     eax,[esi+PE_START]
        mov     [edi+NB_START],eax
        mov     eax,[esi+PE_SECS]
        mov     [edi+NB_SECS],eax
        add     edi,NB_LEN
        inc     dword ptr [nb_n]
nbp_step:
        inc     ebp
        jmp     nbp_one
nbp_done:
        popad
        ret

;---------------------------------------------------------------------
; fd_nblog - the walls around a logical drive: every other link of the
;            chain, and its own boot record.
;
; ITS OWN RECORD IS A WALL, and it is the one a first attempt leaves
; out.  The record stays where it is - the link in front of it points
; at that sector and moving it would mean rewriting the chain - so a
; volume may be moved anywhere inside the container except onto the
; one sector that describes it.  Without this line a /M that moved a
; volume down to the start of its own link would write the volume's
; boot sector straight over the record naming it, and the chain would
; end there.
;---------------------------------------------------------------------
fd_nblog:
        pushad
        mov     dword ptr [nb_n],0
        mov     edi,OFFSET nb_tbl
        mov     esi,[chn_tbl]
        xor     ebp,ebp
nbl_one:
        cmp     ebp,[chn_n]
        jae     nbl_self
        cmp     ebp,[lg_idx]
        je      nbl_step
        cmp     dword ptr [esi+CH_VSTART],0
        jne     nbl_take
        cmp     dword ptr [esi+CH_NEXT],0
        je      nbl_step                ; carries nothing, leads nowhere,
nbl_take:                               ; and so walls nothing in
        mov     eax,[esi+CH_EBR]
        mov     [edi+NB_START],eax
        mov     eax,[esi+CH_EXTENT]
        mov     [edi+NB_SECS],eax
        add     edi,NB_LEN
        inc     dword ptr [nb_n]
nbl_step:
        add     esi,CH_LEN
        inc     ebp
        jmp     nbl_one
nbl_self:
        mov     eax,[pl_sec]
        mov     [edi+NB_START],eax
        mov     dword ptr [edi+NB_SECS],1
        inc     dword ptr [nb_n]
        popad
        ret
;---------------------------------------------------------------------
; fd_lgap - the largest free run INSIDE the extended container, out of
;           the chain fd_chain has already read.  gp_start and gp_len;
;           CF set if there is none.
;
; THE UNIT OF OCCUPANCY IS THE LINK AND NOT THE VOLUME.  A logical
; drive's own boot record sits one track in front of its volume and is
; part of the same thing, so a scan that counted only volumes would
; offer that record's sector as free space - and the next create would
; lay a partition over the middle of a partition table.
;
; A LINK CARRYING NOTHING AND LEADING NOWHERE OCCUPIES NOTHING.  That
; is what a container looks like after its last logical drive has been
; deleted: one record, both entries empty.  Treating it as free is how
; the space comes back, and the create below recognises that it is
; putting its new record exactly where that one was.
;---------------------------------------------------------------------
fd_lgap:
        pushad
        mov     dword ptr [gp_start],0
        mov     dword ptr [gp_len],0
        mov     ebx,[chn_base]          ; EBX = the cursor
        mov     ebp,[chn_base]
        add     ebp,[chn_ext]           ; EBP = one past the container
lg_scan:
        cmp     ebx,ebp
        jae     lg_done

        ; the lowest link starting at or after the cursor
        mov     edx,0FFFFFFFFh          ; EDX = where it starts, none yet
        xor     edi,edi                 ; EDI = what it costs
        xor     eax,eax
        mov     esi,[chn_tbl]
lg_one:
        cmp     eax,[chn_n]
        jae     lg_gap
        cmp     dword ptr [esi+CH_VSTART],0
        jne     lg_live
        cmp     dword ptr [esi+CH_NEXT],0
        je      lg_onenext
lg_live:
        mov     ecx,[esi+CH_EBR]
        cmp     ecx,ebx
        jae     lg_notbehind            ; one that ENCLOSES the cursor
        push    ecx                     ; pushes it past itself
        add     ecx,[esi+CH_EXTENT]
        cmp     ecx,ebx
        jbe     lg_popnext
        mov     ebx,ecx
        pop     ecx
        jmp     lg_scan
lg_popnext:
        pop     ecx
        jmp     lg_onenext
lg_notbehind:
        cmp     ecx,edx
        jae     lg_onenext
        mov     edx,ecx
        mov     edi,[esi+CH_EXTENT]
lg_onenext:
        add     esi,CH_LEN
        inc     eax
        jmp     lg_one

lg_gap:
        mov     ecx,ebp                 ; nothing ahead: to the container's end
        cmp     edx,0FFFFFFFFh
        je      lg_measure
        mov     ecx,edx
        cmp     ecx,ebp
        jbe     lg_measure
        mov     ecx,ebp
lg_measure:
        sub     ecx,ebx
        jbe     lg_step
        cmp     dword ptr [gp_want],0   ; the run the screen named, as in
        je      lg_biggest              ; fd_gap
        mov     eax,[gp_want]
        sub     eax,ebx
        jb      lg_step
        cmp     eax,ecx
        jae     lg_step
        jmp     short lg_take
lg_biggest:
        cmp     ecx,[gp_len]
        jbe     lg_step
lg_take:
        mov     [gp_len],ecx
        mov     [gp_start],ebx
lg_step:
        cmp     edx,0FFFFFFFFh
        je      lg_done
        mov     ebx,edx
        add     ebx,edi
        jmp     lg_scan
lg_done:
        cmp     dword ptr [gp_len],0
        je      lg_none
        popad
        clc
        ret
lg_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fd_lgprev - which record's slot 1 will have to point at lg_ebr.
;             lg_prev = that record's sector, or 0 when the new link
;             is the head of the chain.  EBP = the index the new link
;             will have, which is the number it will be reported as.
;
; A LINK BEING OVERWRITTEN IS NOT ITS OWN PREDECESSOR.  The comparison
; is strict: when the new record lands exactly where an empty one was,
; the walk stops in front of it and the new link inherits its number
; and its place in the list.
;---------------------------------------------------------------------
fd_lgprev:
        push    eax
        push    ecx
        push    esi
        mov     dword ptr [lg_prev],0
        xor     ebp,ebp
        xor     eax,eax
        mov     esi,[chn_tbl]
lp_one:
        cmp     eax,[chn_n]
        jae     lp_done
        mov     ecx,[esi+CH_EBR]
        cmp     ecx,[lg_ebr]
        jae     lp_done
        mov     [lg_prev],ecx
        inc     ebp
        add     esi,CH_LEN
        inc     eax
        jmp     lp_one
lp_done:
        pop     esi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; /C d:n with n above 4 - ADD A LOGICAL DRIVE.
;
; THE NUMBER TYPED IS NOT WHERE IT GOES.  A primary partition is put
; in the slot it is named for, because the four slots are four fixed
; places in one sector.  A logical drive has no such thing: its place
; in the chain is decided by where the free space is, so 5 and 9 both
; mean "make one", and what it turns out to be numbered is printed
; rather than demanded.  Naming one that already exists is still a
; refusal - that is a typing mistake with a delete behind it.
;=====================================================================
fd_crlog:
        call    fd_loadchain
        ; A NUMBER THAT NAMES A LINK CARRYING NOTHING IS FREE.  An
        ; emptied container still has one record in it - that is what
        ; a delete of the last drive leaves - so counting links would
        ; refuse the first create after the chain was cleared out.
        ; What is taken is a DRIVE, and a link with no volume is not
        ; one.
        mov     eax,[pl_log]
        dec     eax
        cmp     eax,[chn_n]
        jae     fdl_numok
        imul    esi,eax,CH_LEN
        add     esi,[chn_tbl]
        cmp     dword ptr [esi+CH_VSTART],0
        jne     fdn_logtaken
fdl_numok:
        cmp     dword ptr [chn_n],EBR_MAX
        jae     fdn_chainfull

        call    fd_lgap
        jc      fdn_nogapx
        call    fd_spcyl
        mov     [gp_spcyl],ecx

        ; THE RECORD IS WHAT LANDS ON THE CYLINDER BOUNDARY, and the
        ; volume starts one track behind it - which is the master boot
        ; record's relationship to the first primary partition, one
        ; level down and for the same reason.  The head of the chain
        ; is the exception: it has to be exactly where the container
        ; starts, boundary or not, because that is the sector the
        ; table names.
        mov     eax,[gp_start]
        cmp     eax,[chn_base]
        je      fdl_ebrok
        mov     ecx,[gp_spcyl]
        call    fd_cylup
fdl_ebrok:
        mov     [lg_ebr],eax
        movzx   edx,word ptr [dsk_rec+DSK_SPT]
        or      edx,edx
        jnz     fdl_havespt
        mov     edx,1                   ; a geometry with no track length
fdl_havespt:
        add     eax,edx
        mov     [lg_start],eax

        mov     edx,[gp_start]
        add     edx,[gp_len]            ; one past the free run
        sub     edx,[lg_start]
        jbe     fdn_nogapx              ; the alignment ate all of it
        mov     [lg_len],edx

        cmp     dword ptr [fd_nargs],2
        jb      fdl_havelen
        mov     esi,OFFSET fd_args + FD_ARGLEN
        call    fd_atosz
        jc      fdn_usage
        or      eax,eax
        jz      fdn_usage
        cmp     eax,[lg_len]
        ja      fdl_toobig
        mov     [lg_len],eax
fdl_havelen:
        mov     eax,[lg_start]
        add     eax,[lg_len]
        mov     ecx,[gp_spcyl]
        call    fd_cyldn
        sub     eax,[lg_start]
        jbe     fdn_tooshort
        mov     [lg_len],eax

        cmp     dword ptr [fd_nargs],3
        jb      fdl_autotype
        mov     esi,OFFSET fd_args + FD_ARGLEN*2
        call    fd_atohex
        jc      fdn_usage
        or      al,al
        jz      fdn_usage
        mov     [cr_type],al
        jmp     fdl_havetype
fdl_autotype:
        mov     eax,[lg_start]
        mov     edx,[lg_len]
        call    fd_newtype
        mov     [cr_type],al
fdl_havetype:
        call    fd_lgprev               ; -> lg_prev, EBP = its number
        mov     [lg_idx],ebp            ; and out of a register, because the
                                        ; report below is a long way of calls

;--- say what it would be --------------------------------------------
        call    crlf
        mov     edx,OFFSET msg_crlog
        call    puts
        mov     eax,[lg_idx]
        add     eax,5
        call    put1
        mov     edx,OFFSET msg_cron
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_crat
        call    puts
        mov     eax,[lg_start]
        call    put1
        mov     edx,OFFSET msg_plus
        call    puts
        mov     eax,[lg_len]
        call    put1
        mov     edx,OFFSET msg_crsec
        call    puts
        mov     eax,[lg_len]
        call    sec2mb
        call    put1
        mov     edx,OFFSET msg_crmb
        call    puts
        movzx   eax,byte ptr [cr_type]
        call    put2
        call    sp1
        movzx   eax,byte ptr [cr_type]
        call    typename
        call    crlf
        mov     edx,OFFSET msg_crebr
        call    puts
        mov     eax,[lg_ebr]
        call    put1
        mov     edx,OFFSET msg_crebr2
        call    puts

        mov     eax,[lg_start]
        add     eax,[lg_len]
        cmp     eax,[phy_chs]
        jbe     fdl_inreach
        mov     edx,OFFSET msg_pastchs
        call    puts
fdl_inreach:
        mov     edx,OFFSET msg_crthen
        call    puts

        cmp     byte ptr [dk_armed],0
        jne     fdl_go
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done

;--- and do it -------------------------------------------------------
fdl_go:
        ; The pointer the new record will inherit, taken from the one
        ; that is about to point at it.  A create in the middle of a
        ; chain - into the space a delete left - has to carry the rest
        ; of the list forward, and those sixteen bytes already say
        ; exactly what the rest of the list is.
        mov     edi,OFFSET chn_save
        xor     eax,eax
        mov     ecx,PE_LEN / 4
        cld
        rep     stosd
        cmp     dword ptr [lg_prev],0
        je      fdl_nohead
        mov     ebx,[lg_prev]
        mov     edi,[ebr_buf]
        call    fd_secrd
        jc      fdn_rderr
        mov     esi,[ebr_buf]
        add     esi,PART_OFF + PE_LEN
        mov     edi,OFFSET chn_save
        mov     ecx,PE_LEN / 4
        cld
        rep     movsd
fdl_nohead:

        ; the new record, built from nothing
        mov     edi,[eb2_buf]
        xor     eax,eax
        mov     ecx,SEC_BYTES / 4
        cld
        rep     stosd
        mov     edi,[eb2_buf]
        mov     word ptr [edi+1FEh],MBR_SIG
        push    edi
        add     edi,PART_OFF
        mov     al,[cr_type]
        mov     ebx,[lg_start]
        mov     ecx,[lg_len]
        mov     edx,[lg_ebr]            ; slot 0 is measured from HERE
        call    fd_putent
        pop     edi
        push    edi
        add     edi,PART_OFF + PE_LEN
        mov     esi,OFFSET chn_save
        mov     ecx,PE_LEN / 4
        cld
        rep     movsd
        pop     edi
        mov     ebx,[lg_ebr]
        call    fd_secwr
        jc      fdn_wrerr

        ; and only now the pointer to it
        cmp     dword ptr [lg_prev],0
        je      fdl_crdone
        mov     edi,[ebr_buf]
        add     edi,PART_OFF + PE_LEN
        mov     al,[chn_ctype]          ; the container's own spelling
        mov     ebx,[lg_ebr]
        mov     ecx,[lg_start]
        add     ecx,[lg_len]
        sub     ecx,[lg_ebr]            ; the whole link, record and all
        mov     edx,[chn_base]          ; slot 1 is measured from THERE
        call    fd_putent
        mov     byte ptr [edi+PE_BOOT],0
        mov     ebx,[lg_prev]
        mov     edi,[ebr_buf]
        call    fd_secwr
        jc      fdn_wrerr
fdl_crdone:
        mov     edx,OFFSET msg_crdone
        call    puts
        jmp     fd_done

fdl_toobig:
        mov     eax,[lg_len]
        mov     [cr_len],eax
        jmp     fdn_toobig

;=====================================================================
; /D d:n with n above 4 - REMOVE A LOGICAL DRIVE.
;
; TWO CASES, AND ONE SECTOR WRITTEN EITHER WAY.
;
; Any link but the first: the record in front of it takes over its
; slot 1 VERBATIM.  Those sixteen bytes are already measured from the
; container and already describe the link that is about to become the
; next one, so copying them is not a shortcut - it is the exactly
; correct answer, and it preserves whatever convention the tool that
; built the chain used for the length field.
;
; The first link: its record cannot be skipped, because the master
; boot record names it and the whole chain is measured from it.  So
; the head STAYS and takes over the second link instead - slot 0
; rewritten to describe the second volume, from this record now, so
; the offset changes; slot 1 copied verbatim.  The second record
; becomes an abandoned sector inside the free space, which is what it
; was going to be anyway.
;
; And if it was the only one, both entries go to zero: a container
; with one empty record in it, which is exactly what fd_chainbusy has
; always called free.
;=====================================================================
fd_dellog:
        call    fd_loadchain
        mov     ebp,[pl_log]
        dec     ebp
        cmp     ebp,[chn_n]
        jae     fdn_nolog
        mov     [lg_idx],ebp
        imul    esi,ebp,CH_LEN
        add     esi,[chn_tbl]
        cmp     dword ptr [esi+CH_VSTART],0
        je      fdn_nolog

        mov     eax,[esi+CH_EBR]
        mov     [lg_ebr],eax
        mov     eax,[esi+CH_VSTART]
        mov     [lg_start],eax
        mov     eax,[esi+CH_VSECS]
        mov     [lg_len],eax
        mov     al,[esi+CH_TYPE]
        mov     [cr_type],al
        mov     eax,[esi+CH_NEXT]
        mov     [lg_next],eax

;--- say what it would be --------------------------------------------
        call    crlf
        mov     edx,OFFSET msg_dellogp
        call    puts
        mov     eax,[lg_idx]
        add     eax,5
        call    put1
        mov     edx,OFFSET msg_cron
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_crat
        call    puts
        mov     eax,[lg_start]
        call    put1
        mov     edx,OFFSET msg_plus
        call    puts
        mov     eax,[lg_len]
        call    put1
        mov     edx,OFFSET msg_crsec
        call    puts
        mov     edx,OFFSET msg_deltype
        call    puts
        movzx   eax,byte ptr [cr_type]
        call    put2
        call    sp1
        movzx   eax,byte ptr [cr_type]
        call    typename
        call    crlf

        cmp     dword ptr [lg_next],0
        je      fdl_dlast
        mov     edx,OFFSET msg_delrenum
        call    puts
fdl_dlast:

        ; the letter it has until the machine restarts
        call    vol_load
        movzx   ebx,byte ptr [pl_drive]
        sub     ebx,80h
        mov     eax,[lg_start]
        call    vol_letter
        jc      fdl_noletter
        push    eax
        mov     edx,OFFSET msg_delltr
        call    puts
        pop     eax
        call    putc
        mov     al,':'
        call    putc
        mov     edx,OFFSET msg_delltr2
        call    puts
fdl_noletter:
        mov     edx,OFFSET msg_delgone
        call    puts

        cmp     byte ptr [dk_armed],0
        jne     fdl_delgo
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done

fdl_delgo:
        cmp     dword ptr [lg_idx],0
        je      fdl_delhead

;--- any link but the first: splice it out ---------------------------
        mov     ebx,[lg_ebr]
        mov     edi,[eb2_buf]
        call    fd_secrd
        jc      fdn_rderr
        mov     esi,[eb2_buf]
        add     esi,PART_OFF + PE_LEN
        mov     edi,OFFSET chn_save
        mov     ecx,PE_LEN / 4
        cld
        rep     movsd

        imul    esi,dword ptr [lg_idx],CH_LEN
        add     esi,[chn_tbl]
        sub     esi,CH_LEN              ; the record in front of it
        mov     ebx,[esi+CH_EBR]
        mov     [lg_prev],ebx
        mov     edi,[ebr_buf]
        call    fd_secrd
        jc      fdn_rderr
        mov     edi,[ebr_buf]
        add     edi,PART_OFF + PE_LEN
        mov     esi,OFFSET chn_save
        mov     ecx,PE_LEN / 4
        cld
        rep     movsd
        mov     ebx,[lg_prev]
        mov     edi,[ebr_buf]
        call    fd_secwr
        jc      fdn_wrerr
        jmp     fdl_deldone

;--- the head, which stays where it is -------------------------------
fdl_delhead:
        mov     ebx,[lg_ebr]
        mov     edi,[ebr_buf]
        call    fd_secrd
        jc      fdn_rderr
        cmp     dword ptr [lg_next],0
        je      fdl_delempty

        ; the second record, for its slot 1 and its volume
        mov     ebx,[lg_next]
        mov     edi,[eb2_buf]
        call    fd_secrd
        jc      fdn_rderr
        mov     esi,[eb2_buf]
        add     esi,PART_OFF + PE_LEN
        mov     edi,OFFSET chn_save
        mov     ecx,PE_LEN / 4
        cld
        rep     movsd

        mov     esi,[chn_tbl]
        add     esi,CH_LEN              ; the link that is taking over
        mov     eax,[esi+CH_VSTART]
        mov     [cr_start],eax
        mov     eax,[esi+CH_VSECS]
        mov     [cr_len],eax
        mov     al,[esi+CH_TYPE]
        mov     [cr_type],al

        mov     edi,[ebr_buf]
        add     edi,PART_OFF
        mov     al,[cr_type]
        mov     ebx,[cr_start]
        mov     ecx,[cr_len]
        mov     edx,[lg_ebr]            ; from THIS record now
        call    fd_putent
        mov     byte ptr [edi+PE_BOOT],0
        mov     edi,[ebr_buf]
        add     edi,PART_OFF + PE_LEN
        mov     esi,OFFSET chn_save
        mov     ecx,PE_LEN / 4
        cld
        rep     movsd
        jmp     fdl_delwr

fdl_delempty:
        mov     edi,[ebr_buf]
        add     edi,PART_OFF
        xor     eax,eax
        mov     ecx,(PE_LEN * 2) / 4
        cld
        rep     stosd                   ; both entries: the container is
                                        ; empty now, and reads that way
fdl_delwr:
        mov     ebx,[lg_ebr]
        mov     edi,[ebr_buf]
        call    fd_secwr
        jc      fdn_wrerr
fdl_deldone:
        mov     edx,OFFSET msg_deldone
        call    puts
        jmp     fd_done

;--- the refusals the chain verbs add --------------------------------
fdn_nocont:
        mov     edx,OFFSET msg_nocont
        call    puts
        jmp     fd_fail
fdn_chbad:
        mov     edx,OFFSET msg_chbad
        call    puts
        jmp     fd_fail
fdn_nolog:
        mov     edx,OFFSET msg_nolog
        call    puts
        jmp     fd_fail
fdn_logtaken:
        mov     edx,OFFSET msg_logtaken
        call    puts
        jmp     fd_fail
fdn_chainfull:
        mov     edx,OFFSET msg_chainfull
        call    puts
        jmp     fd_fail
fdn_nogapx:
        mov     edx,OFFSET msg_nogapx
        call    puts
        jmp     fd_fail
fdn_logactive:
        mov     edx,OFFSET msg_logactive
        call    puts
        jmp     fd_fail
fdn_rderr:
        mov     edx,OFFSET msg_rderr
        call    puts
        jmp     fd_fail
;--- fd_atohex - ESI -> up to two hex digits.  AL = the byte.
;--- CF set if there was not at least one, or something followed them.
fd_atohex:
        push    ecx
        push    edx
        xor     eax,eax
        xor     ecx,ecx
fah_dig:
        movzx   edx,byte ptr [esi]
        cmp     dl,'0'
        jb      fah_end
        cmp     dl,'9'
        jbe     fah_num
        or      dl,20h                  ; fold A-F to a-f
        cmp     dl,'a'
        jb      fah_end
        cmp     dl,'f'
        ja      fah_end
        sub     dl,'a' - 10
        jmp     short fah_take
fah_num:
        sub     dl,'0'
fah_take:
        cmp     ecx,2
        jae     fah_bad
        shl     eax,4
        or      eax,edx
        inc     ecx
        inc     esi
        jmp     short fah_dig
fah_end:
        or      ecx,ecx
        jz      fah_bad
        cmp     byte ptr [esi],0
        jne     fah_bad
        pop     edx
        pop     ecx
        clc
        ret
fah_bad:
        pop     edx
        pop     ecx
        stc
        ret

;--- the refusals these three share ----------------------------------
fdn_usage:
        jmp     fd_usage
fdn_taken:
        mov     edx,OFFSET msg_slottaken
        call    puts
        jmp     fd_fail
fdn_nogap:
        mov     edx,OFFSET msg_nogap
        call    puts
        jmp     fd_fail
fdn_toobig:
        mov     edx,OFFSET msg_toobig
        call    puts
        mov     eax,[cr_len]
        call    put1
        mov     edx,OFFSET msg_crsec
        call    puts
        call    crlf
        jmp     fd_fail
fdn_tooshort:
        mov     edx,OFFSET msg_tooshort
        call    puts
        jmp     fd_fail
fdn_alreadyempty:
        mov     edx,OFFSET msg_slotempty
        call    puts
        jmp     fd_fail
fdn_chainbusy:
        mov     edx,OFFSET msg_chainbusy
        call    puts
        jmp     fd_fail
fdn_wrerr:
        mov     edx,OFFSET msg_wrerr
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

        INCLUDE FDREMAP.INC
        INCLUDE FDTUI.INC

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
        call    tn_find
        call    puts
        ret

;--- tn_find - AL = a type byte -> EDX -> its name, nine wide ---------
tn_find:
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
        pop     ebx
        ret
tn_unknown:
        mov     edx,OFFSET msg_unknown
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
        cmp     byte ptr [tui_on],0
        jne     puts_cap
        mov     eax,0900h
        int     21h
        pop     eax
        ret
; WHILE THE SCREEN IS UP EVERYTHING A VERB SAYS GOES INTO cap_buf, and
; the screen shows it in its panel.  The verbs do not know.
puts_cap:
        push    esi
        mov     esi,edx
pc_one:
        mov     al,[esi]
        cmp     al,'$'
        je      pc_done
        call    cap_put
        inc     esi
        jmp     short pc_one
pc_done:
        pop     esi
        pop     eax
        ret
putc:
        push    eax
        push    edx
        cmp     byte ptr [tui_on],0
        jne     putc_cap
        movzx   edx,al
        mov     eax,0200h
        int     21h
        pop     edx
        pop     eax
        ret
putc_cap:
        call    cap_put
        pop     edx
        pop     eax
        ret
; cap_put - AL onto the end of cap_buf; a full buffer keeps its start
cap_put:
        push    edi
        mov     edi,[cap_len]
        cmp     edi,CAP_MAX
        jae     cap_full
        mov     [cap_buf+edi],al
        inc     dword ptr [cap_len]
cap_full:
        pop     edi
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
        push    edx
        call    fmt_n
        call    puts
        pop     edx
        pop     ebx
        ret

;--- fmt_n - EAX in decimal, right-aligned in EBX, as EDX -> a '$'
;--- string in num_buf.  Everything but EDX comes back as it was.
fmt_n:
        push    eax
        push    ecx
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
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; Data
;=====================================================================
fd_cmd      db  0
fd_mbrok    db  0
fd_menu     db  0                       ; the verbs return instead of exiting
fd_mstack   dd  0                       ; and this is where they return to
fd_misal    db  0                       ; something in this report is not on
                                        ; a cylinder boundary
dk_armed    db  1                       ; cleared only by the screen's dry run
dk_wrote    db  0                       ; set by any write that was tried
dk_drive    db  0
fd_sw       dd  0
fd_nargs    dd  0
fd_ndisks   dd  0
vol_n       dd  0

mbr_buf     dd  0
ebr_buf     dd  0
sec_buf     dd  0
cpy_buf     dd  0
eb2_buf     dd  0
chn_tbl     dd  0

dsk_rec     db  DSK_LEN dup(0)

;--- the volume being planned against, as its BPB describes it -------
fv_valid    db  0
fv_raw      db  0                       ; no boot sector there at all
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
fv_nbad     dd  0                       ; clusters marked bad
fv_media    dd  0
fv_extflags dd  0                       ; FAT32: which FAT is live
fv_rootclus dd  0                       ; FAT32: where the root starts

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
pl_raw      db  0                       ; it has never been formatted
pl_cont     db  0                       ; it is the extended container
pl_cxn      dd  0                       ; and holds this many drives,
pl_cxend    dd  0                       ; the last of its links ending here
cx_ebr      dd  0                       ; a link of it, where it is now
cx_vold     dd  0                       ; a volume of it, where it was
cx_vnew     dd  0                       ; and where it is now
rcm_left    dd  0                       ; rcm_carry: the room left in cpy_buf
rcm_write   db  0                       ; ...and which of its two passes
            db  0, 0, 0
pl_ent      dd  0                       ; -> the entry being planned, in
                                        ; mbr_buf or in ebr_buf
pl_base     dd  0                       ; what its start field is measured
                                        ; from: 0, or its own record
pl_sec      dd  0                       ; the sector to write it back to
pl_lo       dd  0                       ; and the region it may not leave
pl_hi       dd  0
pl_idx      dd  0
pl_log      dd  0                       ; 0 = a primary slot, else the
                                        ; 1-based logical drive named
pl_ostart   dd  0
pl_olen     dd  0
pl_nstart   dd  0
pl_nlen     dd  0
pl_op       dd  0
pl_status   dd  0
pl_room     dd  0
pl_minsz    dd  0                       ; what /P found it could shrink to
pl_maxsz    dd  0                       ; and grow to; 0 when it did not say
rl_needcc   dd  0
rl_maxcc    dd  0
rl_spc      dd  0                       ; the candidate the limits measure
rl_bits     dd  0
rlm_best    dd  0
phy_total   dd  0
phy_chs     dd  0

; /C, /D and /A
gp_start    dd  0                       ; the free gap fd_gap found
gp_len      dd  0
gp_spcyl    dd  0                       ; sectors per cylinder
gp_want     dd  0                       ; 0, or a sector in the free run the
                                        ; screen wants a create to use
ff_start    dd  0                       ; the partition being formatted
ff_len      dd  0
ff_fatend   dd  0                       ; ...where its data area will start
ff_iter     dd  0                       ; cluster sizes ff_layout may still try
cr_start    dd  0                       ; what /C would write
cr_len      dd  0
cr_type     db  0
            db  0,0,0

;--- the extended chain, as fd_chain read it -------------------------
chn_slot    dd  0                       ; which primary slot is the container
chn_base    dd  0                       ; where it starts - the base every
                                        ; slot-1 offset in it is measured from
chn_ext     dd  0                       ; how long it is
chn_cur     dd  0                       ; where the walk is
chn_n       dd  0                       ; links found
chn_st      dd  0                       ; CHN_*
chn_ctype   db  0                       ; the container's own type byte
            db  0,0,0
lg_idx      dd  0                       ; which link in the chain it is
nb_n        dd  0                       ; how many walls there are
nb_tbl      db  NB_MAX * NB_LEN dup(0)
lg_next     dd  0                       ; the record after the one being
                                        ; deleted, 0 when it is the last
chn_save    db  PE_LEN dup(0)           ; one entry, carried from the record
                                        ; it is being unlinked from to the
                                        ; record that inherits it
lg_ebr      dd  0                       ; what /C on a logical would write
lg_start    dd  0
lg_len      dd  0
lg_prev     dd  0                       ; the record whose slot 1 gains it,
                                        ; 0 when it becomes the head

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

msg_usage   db  'Configures a hard disk for use with PM-DOS.',13,10,13,10
            db  'FDISK [/C d:p n tt] [/D d:p] [/A d:p] [/M d:p n] [/S d:p n]',13,10
            db  '       [/STATUS] [/L] [/MBR d]',13,10
            db  '  /STATUS      Provide a report of the current layout',13,10
            db  '  /L           Show logical drives in the extended container',13,10
            db  '  /P d:p       Work with partition p on disk d',13,10
            db  '  /M d:p n     Move partition so that it starts at sector n',13,10
            db  '  /S d:p n     Resize partition to n sectors or n MB, written 40M',13,10
            db  '  /C d:p       Create partition, filling the largest free gap',13,10
            db  '  /C d:p n      ...only n sectors of it, or n MB, and',13,10
            db  '  /C d:p n tt   ...tt as the type byte, in hex',13,10
            db  '  /D d:p       Delete partition',13,10
            db  '  /A d:p       Mark partition as active',13,10
            db  '  /MBR d       Restore Master Boot Record.',13,10,13,10,'$'
msg_status  db  'TATUS',0               ; what follows /S to make /STATUS
msg_disk    db  'Disk $'
msg_sp2     db  '   $'
msg_cyls    db  ' cylinders, $'
msg_heads   db  ' heads, $'
msg_spt     db  ' sectors,$'
msg_mb      db  ' MB',13,10,'$'
msg_mb3     db  ' MB $'
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
msg_badebr  db  '         the extended chain stops at an unreadable '
            db  'record',13,10,'$'
msg_chloop  db  '         the extended chain does not run forward from '
            db  'there',13,10,'$'
msg_chrange db  '         the extended chain points outside its own '
            db  'container',13,10,'$'
msg_alnote  db  '         * begins or ends off a cylinder boundary',13,10,'$'
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
msg_empty2  db  'nothing - the partition is empty',13,10,'$'
msg_raw     db  'not formatted, so no files are stored on the partition$'
msg_cxnone  db  'extended, with no logical drives inside$'
msg_cxsome  db  'extended, holding $'
msg_cxlast  db  ' logical drive(s), the last ending at sector $'
msg_upto    db  ' is the highest cluster,$'
msg_room    db  13,10,13,10,'  Current size  $'
msg_shrink  db  13,10,'  Smallest size      $'
msg_grow    db  13,10,'  Largest size        $'
msg_nolimit db  'not at all - see what is in use above$'
msg_ideal   db  13,10,13,10,'  The smallest cluster this size could take is $'
msg_idealto db  ' sector(s), which',13,10,'  would make the partition $'
msg_idealend db '.  FDISK changes the cluster size only when a',13,10
            db  '  new size needs a different one.',13,10,'$'
msg_nodisk  db  'No such disk.',13,10,'$'
msg_notfat  db  'That partition is not a FAT partition this program can '
            db  'resize.',13,10,'$'

;--- why a move or a resize was refused, RL_NAMELEN bytes a row -------
; EVERY ROW SAYS WHAT TO DO NEXT, which is the difference between an
; error message and a diagnosis.  "Cannot shrink" tells somebody they
; are stuck; "something is stored above the new end, and /P says how
; far down it goes" tells them to defragment and try again.
rl_stname:
        db  'it can be done.                                        $'
        db  'the new range is the range it already has.             $'
        db  'that is not a FAT12, FAT16 or FAT32 volume.            $'
        db  'a sector would not read.                               $'
        db  'a sector would not write - the disk may be damaged.    $'
        db  'something is stored above the new end.  /P says where. $'
        db  'it would run into the partition next to it.            $'
        db  'that is outside the disk or container it lives in      $'
        db  'no legal FAT layout exists at that size.               $'
        db  'writing is held off for this run.                      $'
        db  'no cluster size up to 32K fits that size.              $'
        db  'that size needs FAT32 from FAT16, or the other way.    $'
        db  'a logical drive inside lies past the new end.          $'
        db  'the logical drives inside do not read as a chain.      $'
        db  'some clusters are marked bad, so they cannot grow.     $'
        db  'a chain or a directory is damaged.  Run CHKDSK first.  $'
        db  'there is not enough memory to renumber the clusters.   $'

msg_plus    db  ' + $'
msg_willbe  db  '  it would then be $'
msg_comma2  db  ', $'
msg_spclus2 db  ' sectors/cluster, $'
msg_clus2   db  ' clusters',13,10,'$'
msg_rawonly db  '  not formatted, so only the partition table entry changes',13,10,'$'
msg_cxmove  db  '  Every logical drive inside moves with the partition',13,10,'$'
msg_cxsize  db  '  The logical drives inside stay where they are',13,10,'$'
msg_bootwarn db 13,10,'  NOTE: it is the ACTIVE partition and part of it '
            db  'would sit past',13,10,'  the 1024-cylinder line, where a '
            db  'BIOS with only CHS cannot',13,10,'  boot from it.',13,10,'$'
msg_dryrun  db  13,10,'  Nothing was written.',13,10,'$'
msg_doing   db  13,10,'  Doing it$'
msg_movevol db  13,10,'  moving the volume$'
msg_movecx  db  13,10,'  moving the logical drives$'
msg_fixchain db 13,10,'  updating the records of the logical drives$'
msg_movedata db 13,10,'  moving the data$'
msg_moveroot db 13,10,'  moving the root directory$'
msg_movefat db  13,10,'  writing the file allocation tables$'
msg_wrboot  db  13,10,'  writing the boot record$'
msg_wrtab   db  13,10,'  writing the partition table$'
msg_didit   db  13,10,'  Done.  REBOOT before using the drive. ',13,10,'$'
msg_refused db  13,10,'  Refused: $'

;--- /C, /D and /A ---------------------------------------------------
msg_crprop  db  '  proposed: create partition $'
msg_delprop db  '  proposed: DELETE partition $'
msg_actprop db  '  proposed: make partition $'
msg_cron    db  ' of disk $'
; THE SECOND LINE IS INDENTED UNDER THE FIRST rather than run on to
; it.  All of this fitted on one line while the numbers were small
; and ran off an eighty-column screen the first time a real table was
; printed - which is the width every one of these programs is read
; on, whatever the emulator's window is set to.
msg_crat    db  13,10,'            at sector $'
msg_crsec   db  ' sectors, $'
msg_crmb    db  ' MB, type $'
msg_deltype db  'type $'
msg_actrest db  ' the active one',13,10
            db  '            (every other slot becomes inactive)',13,10,'$'
msg_crthen  db  13,10,'  It will hold no filesystem until FORMAT has '
            db  'been run on it.',13,10,'$'
msg_crdone  db  13,10,'  Created.  REBOOT before using the drive. ',13,10,'$'
msg_deldone db  13,10,'  Deleted.  REBOOT before using the drive. ',13,10,'$'
msg_actdone db  13,10,'  Done.',13,10,'$'
msg_delgone db  13,10,'  Everything stored in it becomes unreachable.  The '
            db  'sectors are not',13,10,'  erased and the volume can be put '
            db  'back by creating a partition',13,10,'  with exactly the '
            db  'start and length printed above.',13,10,'$'
msg_delltr  db  13,10,'  It is drive $'
msg_delltr2 db  ' to the running kernel, and will not be after a restart.$'
msg_pastchs db  13,10,'  NOTE: part of it sits past the 1024-cylinder line, '
            db  'where a BIOS',13,10,'  with only CHS cannot boot from it.  '
            db  'It can still be read.',13,10,'$'

msg_slottaken db 13,10,'  That slot already holds a partition.  Delete it '
            db  'first, or name',13,10,'  an empty slot.',13,10,'$'
msg_slotempty db 13,10,'  That slot is empty.',13,10,'$'
msg_nogap   db  13,10,'  No free space on that disk.  Every sector after '
            db  'the first track',13,10,'  belongs to a partition.',13,10,'$'
msg_toobig  db  13,10,'  Larger than the free space.  The gap, '
            db  'once aligned, is $'
msg_tooshort db 13,10,'  Too small: it would not fill one cylinder.',13,10,'$'
msg_chainbusy db 13,10,'  All logical partitions must be deleted first '
            db  'before',13,10,'  deleting the Extended Partition. ',13,10,'$'
msg_wrerr   db  13,10,'  The partition table would not write.',13,10,'$'


;--- the extended chain ----------------------------------------------
msg_crlog   db  '  proposed: create logical drive $'
msg_dellogp db  '  proposed: DELETE logical drive $'
msg_crebr   db  '  Its own boot record goes at sector $'
msg_crebr2  db  ', one track in',13,10,'  front of it, which is where '
            db  'every tool since 1987 has put one.',13,10,'$'
msg_delrenum db 13,10,'  The logical drives after it each move up one '
            db  'number: what the',13,10,'  report calls 7 will be 6.  '
            db  'Their volumes do not move.',13,10,'$'
msg_nocont  db  13,10,'  That disk has no extended container.  Create one '
            db  'first - a',13,10,'  primary slot with a type of 05, as '
            db  'FDISK /C 0:2 40M 05 - and the',13,10,'  logical drives go '
            db  'inside it.',13,10,'$'
msg_chbad   db  13,10,'  That extended chain does not read the way a chain '
            db  'has to.  FDISK',13,10,'  /L says where the walk stopped.  '
            db  'Nothing here will edit a chain it',13,10,'  could not '
            db  'follow to the end: the repair for one broken link is not',13,10
            db  '  a second one.',13,10,'$'
msg_nolog   db  13,10,'  There is no logical drive of that number.  FDISK '
            db  '/L lists them.',13,10,'$'
msg_logtaken db 13,10,'  That number is already a logical drive.  Delete it '
            db  'first, or name',13,10,'  a number past the last one - a '
            db  'new drive is numbered by where the',13,10,'  free space '
            db  'turns out to be, not by what is typed here.',13,10,'$'
msg_chainfull db 13,10,'  That chain is as long as this program follows.',13,10,'$'
msg_nogapx  db  13,10,'  No free space inside the extended container.  '
            db  'Every sector in it',13,10,'  belongs to a logical drive or '
            db  'to one of their boot records.',13,10,'$'
msg_logactive db 13,10,'  A logical drive cannot be the active one.  Booting '
            db  'looks in the',13,10,'  master boot record, which has four '
            db  'slots and no chain in it - so',13,10,'  the active partition '
            db  'is always one of 1 to 4.',13,10,'$'
msg_rderr   db  13,10,'  A record in the chain would not read, and the '
            db  'surgery needs all',13,10,'  of them.  Nothing was '
            db  'written.',13,10,'$'

;--- /MBR ------------------------------------------------------------
msg_mbrprop db  13,10,'  proposed: replace the boot code of disk $'
msg_mbrhow  db  27h,'s master boot record',13,10
            db  '  The 440 bytes in front of the partition table, and '
            db  'nothing else: the',13,10,'  table below stays, the '
            db  'signature stays, and so do the six bytes at',13,10
            db  '  1B8h where every system since Windows NT writes the '
            db  'disk',27h,'s identity.',13,10,'$'
msg_mbrsame db  13,10,'  The code in it is already this one.  Nothing to '
            db  'do.',13,10,'$'
msg_mbrwarn db  13,10,'  WHATEVER ELSE LIVES IN THOSE BYTES GOES.  A boot '
            db  'manager keeps its',13,10,'  menu in front of the table, '
            db  'and this replaces it with a loader that',13,10,'  reads '
            db  'the active partition and nothing else - so a machine that '
            db  'has',13,10,'  been choosing between two systems will stop '
            db  'offering the choice.',13,10,'$'
msg_mbrdone db  13,10,'  Written.  The partition table was not touched.',13,10,'$'

;--- the screen's format ----------------------------------------------
msg_fmtwill db  13,10,'  The new volume: FAT$'
msg_fmtdoing db 13,10,'  writing the boot sector, the FATs and the root directory$'
msg_fmtdone db  13,10,'  Formatted.  REBOOT before using the drive.',13,10,'$'
msg_fmtnone db  13,10,'  There is no partition there.',13,10,'$'
msg_fmtcont db  13,10,'  An extended partition is formatted one logical '
            db  'drive at a time.',13,10,'$'
msg_fmtsmall db 13,10,'  That partition is too small for a FAT volume.',13,10,'$'

msg_mbadarg db  13,10,'  That is not a partition this program can name.  '
            db  'A disk and a',13,10,'  partition, written 1:3 - and FDISK '
            db  '/L lists them.',13,10,'$'

;--- the master boot record this disk should have, carried inside ----
; GENERATED BY BIN2INC FROM BIN\MBRBOOT.BIN, which is the same 512
; bytes MKDISK lays down on a new disk.  It is carried rather than
; read off the running system for the obvious reason: the disk /MBR
; is there to repair may be the one this program would have read it
; from.  Only MBRW_LEN of it is ever written; the rest of the file is
; the placeholder table, which is exactly what must NOT be copied.
        INCLUDE MBRCODE.INC
img_end:                                ; the file ends here
_FDK    ENDS

; THE CARVED BUFFERS FIRST - fd_entry hands out FD_BSS_LEN bytes from
; bss_start - and then the screen's, which are addressed by name.
_FDB    SEGMENT
        db      FD_BSS_LEN dup(?)
kt_line     db  TUI_COLS + 2 dup(?)
tr_tbl      db  TR_MAX * TR_LEN dup(?)
vw_starts   dd  VW_MAXLINES dup(?)
cap_buf     db  CAP_MAX dup(?)
scr_buf     dw  TUI_CELLS dup(?)
ov_data     db  OV_MAX * SEC_BYTES dup(?)       ; the changes waiting:
ov_lba      dd  OV_MAX dup(?)                   ; the sectors staged,
ov_drv      db  OV_MAX dup(?)                   ; where they go,
jn_tbl      db  JN_MAX * JN_LEN dup(?)          ; and what made them
bss_end     LABEL BYTE
_FDB    ENDS
IF (bss_end - bss_start) NE (FD_BSS_LEN + TUI_BSS_LEN)
        .ERR    TUI_BSS_LEN does not match what _FDB holds
ENDIF
        END
