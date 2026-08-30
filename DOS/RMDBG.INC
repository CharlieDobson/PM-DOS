;=====================================================================
; RMDBG.INC - a trace channel for the side of the handover that has no
;             kernel on it.
;
; IO\DBGSINK.INC watches a guest from inside the V86 monitor and writes
; its ring out when the guest stops.  Neither half of that works here.
; Once PM-DOS has parked there is no monitor, no timer of ours, no
; protected mode and no way back until the program running on RMDOS
; ends - so if it never ends, nothing above 1MB is ever asked a
; question again and the machine simply sits there.  That is exactly
; the case worth instrumenting: Windows starting and not finishing.
;
; SO THIS ONE WRITES AS IT GOES, and accepts what that costs.  Every
; INT 21h RMDOS serves appends sixteen bytes to a ring and then puts
; the whole 4K buffer on the disk.  It is an absurd amount of I/O for
; one DOS call and it does not matter: the disk is emulated, the
; alternative is a buffer nobody ever reads, and a trace that survives
; a hang is worth more than a fast one that does not.
;
; IT GOES ON THE SECOND DISK, in the same gap between the master boot
; record and the first partition that DBGSINK uses, but on drive 81h
; rather than 80h.  Two sinks on one disk would be one sink:
; PM-DOS dumps its ring at LBA 1 of drive 80h when it picks the machine
; back up, and that is the same moment this trace becomes readable, so
; sharing the sectors would mean losing whichever half explained the
; other.  Written through the BIOS's own INT 13h, which needs nothing
; of RMDOS's beyond a stack - see rdbg_flush for which of its two write
; calls, and why the choice is not the obvious one.
;
; The buffer is part of the image, so it arrives zeroed: PM-DOS reloads
; RMDOS.SYS from disk on every park, and there is nothing to
; initialise.
;
; Included only when DBG_LPT is defined, like everything else here.
;
; AND THE NAMING RULE FROM DBGSINK.INC APPLIES HERE TOO, because it was
; broken here first: wasm folds case, so a LABEL and a CONSTANT that
; differ only in case are one symbol and the constant wins.  The count
; below was rdbg_2fn next to a RDBG_2FN, which is the same name twice.
; This time the assembler said so - "invalid instruction operands" on
; an INC - where in DBGSINK.INC the same mistake assembled cleanly and
; wrote over the interrupt vector table.  Do not rely on being told:
; no constant in this file may spell a label with the case changed.
;=====================================================================

RDBG_CHS        EQU     0002h           ; cylinder 0, sector 2 - see below
RDBG_SEC        EQU     8
RDBG_LEN        EQU     RDBG_SEC * 512
RDBG_HDR        EQU     128
RDBG_ENT        EQU     16
RDBG_2FN        EQU     32              ; INT 2Fh AX values kept
RDBG_MAX        EQU     (RDBG_LEN - RDBG_HDR - RDBG_2FN * 2) / RDBG_ENT

;--- Milestone bits, so that a hang before the first DOS call is still
;--- a report rather than a silence.
RDBGM_ENTRY     EQU     0               ; rd_entry, header checked
RDBGM_VECS      EQU     1               ; the vectors are ours
RDBGM_VOL       EQU     2               ; per-drive geometry
RDBGM_SYS       EQU     3               ; SDA, list of lists, CDS
RDBGM_SFT       EQU     4               ; the handle table
RDBGM_XMS       EQU     5               ; extended memory and A20
RDBGM_RESUME    EQU     6               ; about to IRET into the guest
RDBGM_RUN       EQU     7               ; about to start a program instead
RDBGM_BACK      EQU     8               ; rd_goback - the excursion ended

;---------------------------------------------------------------------
; rdbg_flush - the buffer to the disk.  Every register preserved: this
;              is called from the middle of other people's work.
;
; CHS, AND NOT BECAUSE THE MACHINE IS OLD.  A sector this low is at
; cylinder 0, head 0 on ANY geometry with seventeen or more sectors per
; track, which is every hard disk that has ever existed, so the
; conversion needs nothing the BIOS would have to tell us and the call
; cannot fail for a reason we would then be unable to report.  The
; extended write - AH=43h with a packet - was tried first and wrote
; nothing at all, which is a failure with no symptom: an absent trace
; and a hang before the first write look exactly alike.
;
; AND THE SECTOR NUMBER IS DELIBERATELY NOT THE FIRST ONE FREE.
; Sector 2 - which is LBA 1, and is what the arithmetic above says -
; landed on LBA 0 and took the partition table with it.  Whatever the
; off-by-one is, it is one, and it is cheaper to stay ten sectors away
; from anything that matters than to find out whose it is: the reader
; SCANS the first few sectors for the signature rather than being told
; where to look.
;---------------------------------------------------------------------
rdbg_flush:
        pusha
        push    ds
        push    es
        mov     ax,cs
        mov     ds,ax
        mov     es,ax
        mov     bx,OFFSET rdbg_buf
        mov     ax,0300h + RDBG_SEC     ; write, RDBG_SEC sectors
        mov     cx,RDBG_CHS
        mov     dx,8100h                ; head 0, drive 81h
        int     13h
        pop     es
        pop     ds
        popa
        ret

;---------------------------------------------------------------------
; rdbg_saveframe - keep a copy of the register frame the guest is about
;                  to be resumed on.
;
; The one thing that cannot be deduced afterwards.  If the guest stops
; without making a single DOS call then either it never ran or it ran
; somewhere it should not have, and those two look identical from
; outside; the frame says which.
;---------------------------------------------------------------------
rdbg_saveframe0:
        mov     di,OFFSET rdbg_frame0
        jmp     short rdbg_savecommon
rdbg_saveframe:
        mov     di,OFFSET rdbg_frame
rdbg_savecommon:
        pusha
        push    ds
        push    es
        mov     ax,cs
        mov     ds,ax
        mov     es,ax
        mov     si,OFFSET h_frame
        mov     cx,RMI_LEN
        cld
        rep     movsb
        pop     es
        pop     ds
        popa
        ret

;---------------------------------------------------------------------
; rdbg_i2frec - one INT 2Fh, recorded from a handler that has not built
;               a frame and is about to compare the flags it arrived
;               with.  PUSHF/POPF around the lot: INT 13h below sets
;               carry, and i2F's next instruction is a CMP whose result
;               would then be a report about the disk.
;---------------------------------------------------------------------
rdbg_i2frec:
        pushf
        push    ax
        push    bx
        push    si
        push    ds
        mov     bx,cs
        mov     ds,bx
        mov     bx,word ptr [rdbg_2fseen]
        cmp     bx,RDBG_2FN
        jae     ri_full                 ; keep the FIRST ones: what Windows
        shl     bx,1                    ; asks for early is the question
        mov     si,OFFSET rdbg_2fax
        add     si,bx
        mov     [si],ax
ri_full:
        inc     word ptr [rdbg_2fseen]
        call    rdbg_flush
        pop     ds
        pop     si
        pop     bx
        pop     ax
        popf
        ret

;---------------------------------------------------------------------
; rdbg_mark - AL = a milestone bit.  Sets it and writes the buffer, so
;             the last bit set is where the machine stopped.
;---------------------------------------------------------------------
rdbg_mark:
        push    ax
        push    bx
        push    cx
        push    ds
        mov     bx,cs
        mov     ds,bx
        mov     cl,al
        mov     bx,1
        shl     bx,cl
        or      word ptr [rdbg_marks],bx
        call    rdbg_flush
        pop     ds
        pop     cx
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
; rdbg_rec - one INT 21h.  Called from i21 with BP already pointing at
;            the copied register frame, which is on RMDOS's own stack
;            and therefore addressed through SS - so DS is free to be
;            our own segment and both are the same segment anyway.
;---------------------------------------------------------------------
rdbg_rec:
        push    ax
        push    bx
        push    si
        push    ds
        mov     ax,cs
        mov     ds,ax

        mov     bx,word ptr [rdbg_head]
        cmp     bx,RDBG_MAX
        jb      rr_slot
        xor     bx,bx                           ; round the ring
rr_slot:
        mov     si,bx
        shl     si,4
        add     si,OFFSET rdbg_ents

        mov     ax,[bp+F_AX]
        mov     [si+0],ax
        mov     ax,[bp+F_BX]
        mov     [si+2],ax
        mov     ax,[bp+F_CX]
        mov     [si+4],ax
        mov     ax,[bp+F_DX]
        mov     [si+6],ax
        mov     ax,[bp+F_CS]
        mov     [si+8],ax
        mov     ax,[bp+F_IP]
        mov     [si+10],ax
        mov     ax,[bp+F_DS]
        mov     [si+12],ax
        mov     ax,word ptr [rdbg_count]
        mov     [si+14],ax

        inc     bx
        mov     word ptr [rdbg_head],bx
        inc     word ptr [rdbg_count]
        call    rdbg_flush

        pop     ds
        pop     si
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
; The buffer, which IS the disk image of the trace - header first, so
; that the reader on the other end needs to know one offset and not
; two.  Nothing here is initialised at run time; see the head of this
; file for why it does not have to be.
;---------------------------------------------------------------------
rdbg_buf:
rdbg_sig    db  'RMDBG',0,0,0           ; +00
rdbg_count  dw  0                       ; +08  INT 21h calls served, total
rdbg_head   dw  0                       ; +0A  the next slot to write
rdbg_marks  dw  0                       ; +0C  RDBGM_* bits
rdbg_2fseen    dw  0                       ; +0E  INT 2Fh calls seen
rdbg_frame0 db  RMI_LEN dup (0)         ; +10  the frame AS IT ARRIVED
rdbg_frame  db  RMI_LEN dup (0)         ; +40  ...and as it is when we use it
rdbg_pad    db  16 dup (0)              ; +70
rdbg_2fax   dw  RDBG_2FN dup (0)        ; +80  the first few of their AX
rdbg_ents   db  (RDBG_LEN - RDBG_HDR - RDBG_2FN * 2) dup (0)
