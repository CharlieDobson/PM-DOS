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
RDBG_FOOT       EQU     40              ; CHS sector of footprint 0 - LBA 39
RDBG_SEC        EQU     32              ; LBA 1 to 32.  Sixteen was not
                                        ; enough once the question became
                                        ; "what did the shell open, in what
                                        ; order, over the last fifty calls"
RDBG_LEN        EQU     RDBG_SEC * 512
RDBG_HDR        EQU     160             ; 128 until v0.55, when the handle
                                        ; table became the question
RDBG_ENT        EQU     32
RDBG_2FN        EQU     32              ; INT 2Fh AX values kept
RDBG_STRN       EQU     8               ; strings kept - the FIRST ones.
                                        ; Eight now, not twenty-four: the
                                        ; start-up sequence is known and
                                        ; written down, and the space is
                                        ; worth more as CALL RECORDS - the
                                        ; question is what the shell does,
                                        ; and that is four hundred calls in.
RDBG_STRL       EQU     128
RDBG_XN         EQU     24              ; XMS calls kept - the FIRST ones.
RDBG_LSTN       EQU     24              ; names kept - the LAST ones.
                                        ; Both rings earn their keep: the
                                        ; first names say how a start-up
                                        ; got where it did, and the last
                                        ; say what a program that will not
                                        ; stop is doing over and over.
                                        ; Windows reached Program Manager
                                        ; and then read one file forever,
                                        ; and a ring of firsts cannot name
                                        ; a file opened for the four
                                        ; hundredth time.
RDBG_XL         EQU     10              ; 40 was chosen when the interesting
                                        ; traffic was WIN.COM's opening
                                        ; questions; the extender then made
                                        ; 67 calls and the tail - everything
                                        ; after it had taken the memory, and
                                        ; so everything nearest the fault -
                                        ; fell off the end unseen.  96 covers
                                        ; a whole standard-mode start and
                                        ; still leaves 124 INT 21h slots.
RDBG_EOFF       EQU     RDBG_HDR + RDBG_2FN * 2 + RDBG_STRN * RDBG_STRL + RDBG_XN * RDBG_XL + RDBG_LSTN * RDBG_STRL
RDBG_MAX        EQU     (RDBG_LEN - RDBG_EOFF) / RDBG_ENT

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

;--- rdbg_init - point the bounce buffer at the top of our own block.
;
; THE TRACE LIVES WHERE THE DISK CANNOT REACH IT.  On this BIOS an INT
; 13h WRITE from above the megabyte returns success and moves nothing -
; reads into it are fine, which is why the file system never noticed -
; so a trace written from the HMA goes nowhere and the channel is
; silent for a reason that has nothing to do with what it was watching.
; Parked low, which is where RMDOS goes now, the buffer is already
; somewhere the BIOS can see, and the copy below is only a copy because
; the HMA fallback still exists.
;
; AND IT IS NOT ALLOCATED ANY MORE.  It used to ask mem_alloc for four
; kilobytes of the arena, which is honest and is the wrong answer for
; the same reason park_loadrm's old arithmetic was: every block handed
; out after it moves, so the machine the trace build measures is not
; the machine the release build runs.  park_loadrm now takes a whole
; 64K segment whatever the image weighs, and the image is half of it,
; so the buffer goes in the top of that segment and costs nothing that
; was not already ours.  A debug build and a release build now put
; Windows at the same address.
;
; C000h is chosen with room to spare: the largest build that has ever
; carried this file is 33436 bytes, and the stack is at the end of the
; image, not the end of the segment.
RDBG_BNCPARA    EQU     0C00h           ; paragraphs into our own segment

rdbg_init:
        push    ax
        mov     ax,cs
        cmp     ax,0F000h
        jae     ri_none                 ; parked in the HMA: there is no
                                        ; room above us and the write would
                                        ; not land anyway
        add     ax,RDBG_BNCPARA
        mov     word ptr [rdbg_low],ax
ri_none:
        pop     ax
        ret

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
; AND THERE IS NO OFF-BY-ONE.  This comment used to say that sector 2
; landed on LBA 0 and took the partition table with it, so the reader
; had better SCAN rather than be told where to look.  With the drive
; number right, sector 2 lands on LBA 1, which is what the arithmetic
; says and what PROBE13.COM measures.  The story was invented to
; explain a symptom that had a different cause, and it outlived it.
;
; A FOOTPRINT PER FLUSH, because the main write is 8K of buffer to the
; same sixteen sectors every time and therefore only ever shows the
; LAST flush that SUCCEEDED.  If flushes stop landing part way through
; a run, the disk holds an early state and says nothing about how many
; came after it or why they failed - which is exactly the reading that
; sent three releases after the wrong theory.  So each flush also
; drops one sector of its own, further up the gap, carrying its number
; and the status of the write before it.
;
; AND THE FOOTPRINTS WRAP NOW.  They used to be the FIRST sixteen,
; which answers "did the channel ever work" and not "is it still
; working", and those are different questions with the same silence.
; A trace build that gets 114 DOS calls into a Windows start-up in
; four hundred seconds is either a machine crawling or a machine that
; stopped at call 114 with a channel that stopped with it, and the
; first sixteen footprints - all of them from the first five calls -
; cannot tell the two apart.  The last sixteen can: their numbers and
; their DOS-call counts say how far the run had got when the channel
; was last alive.
;---------------------------------------------------------------------
rdbg_flush:
        pusha
        push    ds
        push    es
        mov     ax,cs
        mov     ds,ax
        mov     es,ax
        call    rdbg_jsnap              ; the handle table, as it is NOW
        mov     word ptr [rdbg_pseg],cs  ; WHERE THIS DOS ACTUALLY IS.
        mov     ax,word ptr [rdbg_low]  ; The Windows kernel scans for
        mov     word ptr [rdbg_pbnc],ax ; the SFT and then builds a
                                        ; descriptor to reach it, so the
                                        ; park address is not a detail -
                                        ; it is an input to the next bug.
        mov     bx,OFFSET rdbg_buf
        cmp     word ptr [rdbg_low],0
        je      rf_write
        mov     si,OFFSET rdbg_buf      ; down to where the BIOS can see it
        mov     es,word ptr [rdbg_low]
        xor     di,di
        mov     cx,RDBG_LEN / 2
        cld
        rep     movsw
        xor     bx,bx                   ; ES:BX is the copy, not the original
rf_write:
        mov     ax,0300h + RDBG_SEC     ; write, RDBG_SEC sectors
        mov     cx,RDBG_CHS
        mov     dx,0081h                ; DH = head 0, DL = drive 81h
;
; AND NOT 8100h, WHICH IS WHAT IT SAID FOR THREE RELEASES.  DH is the
; head and DL is the drive; 8100h asks for head 129 of drive 0, which
; is the FLOPPY, and there is one in the machine so the call did not
; even fail in an interesting way.  The comment on the line said "head
; 0, drive 81h" the whole time and was read, every time, as agreeing
; with it.  The HMA, the bounce buffer, AH=43h against AH=03h - three
; releases of theory about why this write did not land, and the write
; was never addressed to the disk it was supposed to land on.
        int     13h
        mov     word ptr [rdbg_lastax],ax
        pushf
        pop     ax
        mov     word ptr [rdbg_lastfl],ax

;--- and the footprint, which is the part that survives -------------
; Written from the SAME bounce buffer, whose first sixteen bytes are
; overwritten for the purpose: the copy above is already down there,
; the 4K of it after byte 16 is of no interest to this sector, and
; borrowing it costs no more memory and no more code than a second
; buffer would.
        mov     bx,word ptr [rdbg_nflush]
        inc     word ptr [rdbg_nflush]
        mov     word ptr [rdbg_fnum],bx         ; the NUMBER, unmasked
        and     bx,0Fh                          ; ...and the SLOT, which wraps
        push    bx
        push    es
        mov     ax,word ptr [rdbg_low]
        or      ax,ax
        jz      rf_nolow
        mov     es,ax
        xor     di,di
        jmp     short rf_stamp
rf_nolow:
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rdbg_buf
rf_stamp:
        mov     word ptr es:[di+0],'LF'         ; 'FL'
        mov     word ptr es:[di+2],'SU'         ; 'US'
        mov     word ptr es:[di+4],' H'         ; 'H '
        mov     ax,word ptr [rdbg_fnum]
        mov     word ptr es:[di+6],ax           ; which flush this is
        mov     ax,word ptr [rdbg_lastax]
        mov     word ptr es:[di+8],ax           ; ...and how the write went
        mov     ax,word ptr [rdbg_lastfl]
        mov     word ptr es:[di+10],ax
        mov     ax,word ptr [rdbg_marks]
        mov     word ptr es:[di+12],ax          ; how far RMDOS had got
        mov     ax,word ptr [rdbg_count]
        mov     word ptr es:[di+14],ax
        mov     ax,0301h                        ; write, one sector
        mov     cx,bx
        add     cx,RDBG_FOOT                    ; CH=0 already: BX < 16
        mov     dx,0081h
        mov     bx,di
        int     13h
        pop     es
        pop     bx
rf_done:
        pop     es
        pop     ds
        popa
        ret

rdbg_low    dw  0                       ; the bounce buffer's segment, or 0
rdbg_nstr   dw  0                       ; strings captured, ever
rdbg_lslot  dw  0                       ; the wrapping ring's next slot
rdbg_slot   dw  0                       ; the entry awaiting its result
rdbg_nflush dw  0                       ; flushes attempted, ever
rdbg_fnum   dw  0                       ; ...and the one being stamped
rdbg_lastax dw  0                       ; the previous write's AX...
rdbg_scand  db  0                       ; the memory scan runs once
rdbg_execd  db  0                       ; ...and not until the kernel is IN
RDBG_SCANN  EQU 12                      ; hits recorded
RDBG_SCANL  EQU 8
rdbg_scanmsg db 'SFTSCAN '
rdbg_lastfl dw  0                       ; ...and its flags
rdbg_xah    db  0                       ; the XMS function asked for,
rdbg_xdx    dw  0                       ; and its DX, held until it returns

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

;--- rdbg_jsnap - the current process's handle table, counted.
;
; Called from the flush, so the picture is of the moment the trace was
; written and not of some earlier moment nobody can name.  Windows
; switches the PSP with AH=50h for every task it runs, so "the process
; that ran out of handles" is a question with a different answer every
; few calls, and the answer is here.
rdbg_jsnap:
        pusha
        push    es
        mov     ax,[rd_psp]
        mov     word ptr [rdbg_lpsp],ax
        mov     es,ax
        call    jft_ptr                 ; -> ES:DI the table, CX its size
        mov     word ptr [rdbg_jcnt],cx
        mov     word ptr [rdbg_joff],di
        mov     ax,es
        mov     word ptr [rdbg_jseg],ax
        xor     bx,bx
        xor     dx,dx
js_loop:
        cmp     bx,cx
        jae     js_done
        cmp     byte ptr es:[bx+di],0FFh
        je      js_next
        inc     dx
js_next:
        inc     bx
        jmp     short js_loop
js_done:
        mov     word ptr [rdbg_jused],dx
        pop     es
        popa
        ret

;--- rdbg_jfail - an open was refused for want of a handle.  Take the
;--- picture HERE, where it happened: rdbg_jsnap runs at the flush, and
;--- the flush is up to thirty-two calls later with a different PSP
;--- current - Windows switches the PSP with 50h for every task it
;--- runs, and the one that ran out is not the one still standing.
rdbg_jfail:
        pusha
        push    ds
        push    es
        mov     ax,cs
        mov     ds,ax
        inc     word ptr [rdbg_nfail]
        cmp     word ptr [rdbg_nfail],1
        jne     jf_out                  ; the FIRST one is the one that
                                        ; matters; the other twenty-three
                                        ; are the shell trying again
        mov     ax,[rd_psp]
        mov     word ptr [rdbg_fpsp],ax
        mov     es,ax
        call    jft_ptr
        mov     word ptr [rdbg_fcnt],cx
        xor     bx,bx
        xor     dx,dx
jf_loop:
        cmp     bx,cx
        jae     jf_done
        cmp     byte ptr es:[bx+di],0FFh
        je      jf_next
        inc     dx
jf_next:
        inc     bx
        jmp     short jf_loop
jf_done:
        mov     word ptr [rdbg_fused],dx
jf_out:
        pop     es
        pop     ds
        popa
        ret

;--- rdbg_h67 - somebody asked for a bigger table.  BX = how many.
rdbg_h67:
        push    ax
        push    ds
        mov     ax,cs
        mov     ds,ax
        cmp     word ptr [rdbg_n67],0
        jne     rh_notfirst
        mov     word ptr [rdbg_h67a],bx
rh_notfirst:
        mov     word ptr [rdbg_h67b],bx
        inc     word ptr [rdbg_n67]
        pop     ds
        pop     ax
        ret

;---------------------------------------------------------------------
; rdbg_maybe - flush, but not on every single DOS call.
;
; THE INSTRUMENT WAS COSTING MORE THAN THE MACHINE.  Every INT 21h
; wrote the whole 8K buffer to the disk twice - once for the question
; and once for the answer - which is thirty-two sectors through the
; BIOS, one BIOS call each, per DOS call.  A Windows start-up makes
; thousands of DOS calls and this build got a hundred and fourteen
; into one in four hundred seconds.  A trace that slows its subject by
; two orders of magnitude is not measuring the subject.
;
; Every thirty-second call instead.  The ring holds sixty entries, so
; the disk is never more than half a ring behind the machine and the
; tail is still there to read; the footprints say how far past the
; last flush the run got.  Marks and INT 2Fh still write immediately -
; there are nine of the first and a handful of the second, and they
; are the milestones a hang is located between.
;---------------------------------------------------------------------
RDBG_EVERY      EQU     32              ; DOS calls between flushes

rdbg_maybe:
IFDEF DBG_QUIET
;--- BUILD lptq - RECORD, BUT DO NOT WRITE.
;
; The question this answers is whether the INSTRUMENT is what stops
; the machine.  The trace build gets about a hundred DOS calls into a
; Windows start-up and stays on the splash screen for as long as it is
; left there, at a hundred and one per cent emulated speed - a machine
; that is waiting, not a machine that is working - while the release
; build has Program Manager up in a hundred and fifty seconds.  Cutting
; the flushes by sixteen changed where it stopped by eighteen calls and
; nothing else, so the VOLUME of disk writing is not the cost; this
; build removes the disk writing from the run altogether and asks
; whether an INT 13h from inside a DOS call, at the moment Windows
; takes the video mode, is what the machine is waiting on.
;
; Nothing is written and therefore nothing can be read: the answer to
; this build is a screenshot, not a trace.
        ret
ENDIF
        push    ax
        mov     ax,word ptr [rdbg_count]
        and     ax,RDBG_EVERY - 1
        jnz     rmb_out
        call    rdbg_flush
rmb_out:
        pop     ax
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
        shl     si,5
        add     si,OFFSET rdbg_ents
        mov     word ptr [rdbg_slot],si         ; where the ANSWER goes

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

;--- AND WHAT IT SAID, not just that it said something.
;
; Windows fails, prints one line through AH=09h and exits with code
; FFh, and the line goes to a screen that is in a graphics mode and
; about to be taken away by a reset.  A register trace can show that
; DS:DX pointed somewhere; only the bytes say which of the dozen
; things that could have gone wrong did.  This is v0.48's lesson
; arriving through a different door: the machine was not hanging, it
; was talking to nobody.
;--- 3Dh and 4Bh as well as 09h: what it OPENED and what it RAN.
; A run that ends in one printed line and ten failed opens of the same
; name has told you the complaint and not the subject.  The name is at
; DS:DX for all three functions, which is the only reason this is one
; piece of code and not three.  The slot carries AH in its first byte
; so the reader can say which question each answer belongs to.
        mov     ax,[bp+F_AX]
        cmp     ah,09h
        je      rr_str
        cmp     ah,3Dh
        je      rr_str
        cmp     ah,4Bh
        je      rr_exec
        cmp     ah,3Eh
        je      rr_scan
        jmp     rr_nostr
;
; 3Dh IS OFF AGAIN.  Ten opens of CON crowd everything else out of an
; eight-slot ring, and once it is known that they are all CON they are
; the least informative thing in it.  What is NOT known is which
; program WIN.COM hands the machine to - every call in this trace
; comes back to one segment, which is either one program with a call
; gate in it or two the loader has put in the same place, and those
; are very different situations.
rr_str:
        push    ds
        push    es
        push    si
        push    di
        push    cx
        mov     ax,cs
        mov     es,ax

;--- THE LAST ONES FIRST, in a ring that wraps.  A program stuck in a
;--- loop has nothing to say in a ring of firsts.
;
;--- AND EACH ONE CARRIES THE CALL NUMBER IT HAPPENED ON.  Slot order
;--- is not time order once the ring has wrapped, and working out where
;--- the wrap is by reading the names and deciding which look like
;--- start-up is guesswork - it was got backwards once already, which
;--- turned "the shell reads PROGMAN.INI and then stops" into "the
;--- shell read PROGMAN.INI a while ago and is still loading fonts".
;--- The count says which, and it says how fast.
        mov     ax,word ptr [rdbg_lslot]
        inc     word ptr [rdbg_lslot]
        cmp     word ptr [rdbg_lslot],RDBG_LSTN
        jb      rr_lwrap
        mov     word ptr [rdbg_lslot],0
rr_lwrap:
        shl     ax,7                    ; * RDBG_STRL
        add     ax,OFFSET rdbg_lstrs
        mov     di,ax
        mov     al,byte ptr [bp+F_AX+1]
        stosb
        mov     ax,word ptr [rdbg_count]
        stosw
        push    ds
        mov     si,[bp+F_DX]
        mov     ds,[bp+F_DS]
        mov     cx,RDBG_STRL - 3
        cld
        rep     movsb
        pop     ds
;--- THE FIRST SIXTEEN AND NOT THE LAST SIXTEEN, which is the same
;--- choice rdbg_i2frec made about INT 2Fh and for the same reason:
;--- what a program asks for FIRST is what it is trying to do, and a
;--- ring throws exactly that away.  Ten opens of CON in a row pushed
;--- the name of every file Windows had opened before them out of an
;--- eight-slot ring, including the one that would have said which
;--- kernel it went looking for.
        mov     ax,word ptr [rdbg_nstr]
        cmp     ax,RDBG_STRN
        jae     rr_strfull
        inc     word ptr [rdbg_nstr]
        shl     ax,7                    ; * RDBG_STRL
        add     ax,OFFSET rdbg_strs
        mov     di,ax
        mov     al,byte ptr [bp+F_AX+1] ; AH, so the reader knows which
        stosb
        mov     si,[bp+F_DX]            ; the frame is on SS, which is us,
        mov     ds,[bp+F_DS]            ; so this stays readable after DS goes
        mov     cx,RDBG_STRL - 1
        cld
        rep     movsb
        pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds
        jmp     rr_nostr
rr_strfull:
        pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds
;--- AND IT HAS TO JUMP.  This label used to FALL THROUGH - past a
;--- comment block, which is why it read as an ending - into rr_exec,
;--- whose last instruction is "jmp rr_str".  So the moment the string
;--- ring filled, the twenty-fourth name sent this routine round and
;--- round, five words deeper on the stack each time, and the machine
;--- stopped.
;---
;--- That is the whole of "the debug build is not the release build".
;--- The trace build stalled at the splash screen at about DOS call
;--- 100 and the release build reached the desktop every time, and the
;--- difference between a debug run that got to Program Manager and
;--- one that did not was rdbg_env: one more string, which took the
;--- count from twenty-three to twenty-four and over the edge.  It
;--- was read as a layout sensitivity - forty-eight more bytes of
;--- RMDOS - and a whole release of park_loadrm was rewritten around
;--- that reading.  The 64K park block is worth keeping anyway; the
;--- reason given for it was wrong.
        jmp     rr_nostr

;---------------------------------------------------------------------
; rr_scan - the scan KRNL386 does, run from inside this DOS at the
;           moment the kernel does it.
;
; SFTSCAN.COM is the same rule with a console to print to, and it says
; the table is right: three names at a 30h stride, measured correctly.
; But SFTSCAN runs with nothing else in the machine.  Under Windows
; the scan starts at zero and everything BELOW this DOS is scanned
; first - DOSX, WIN.COM, and the whole of KRNL386's DOS image, which
; the loader put in low memory because the MZ header claims the whole
; file.  A false triple down there is a wrong stride handed to the
; kernel before it ever reaches the real table, and it would be the
; same false triple every time no matter what this image contains -
; which is exactly what the fault looks like.
;
; So this records what the scan SEES, in the order it sees it.  It
; runs once, on the first 3Eh, by which time the kernel has opened its
; five handles and the table is as it will ever be.  The addresses go
; into a string slot marked 09h so the reader prints them as text.
;
; FS carries the window and ES stays on this segment, because DI is
; the output cursor and scasb would want it for the search.
;---------------------------------------------------------------------
rr_exec:
        mov     byte ptr [rdbg_execd],1
        jmp     rr_str

rr_scan:
        cmp     byte ptr [rdbg_execd],0                  ; nothing has been EXECed yet:
        je      rr_nostr                        ; this 3Eh is WIN.COM closing
                                                ; SYSTEM.INI and the kernel is
                                                ; not in memory to be found
        cmp     byte ptr [rdbg_scand],0
        jne     rr_nostr
        mov     byte ptr [rdbg_scand],1
        pushad
        push    ds
        push    es
        push    fs

        mov     ax,cs
        mov     ds,ax
        mov     es,ax
        mov     ax,word ptr [rdbg_nstr]
        cmp     ax,RDBG_STRN
        jae     rsc_out
        inc     word ptr [rdbg_nstr]
        shl     ax,7                    ; * RDBG_STRL
        add     ax,OFFSET rdbg_strs
        mov     di,ax
        mov     al,09h                  ; the reader prints 09h as text
        stosb
        mov     si,OFFSET rdbg_scanmsg
        mov     cx,RDBG_SCANL
        cld
        rep     movsb

        xor     bx,bx                   ; window, in paragraphs
        xor     dx,dx                   ; hits emitted
rsc_win:
        mov     fs,bx
        xor     si,si
rsc_look:
        cmp     si,0FFF0h
        jae     rsc_next
        cmp     byte ptr fs:[si],'C'
        jne     rsc_bump
        cmp     byte ptr fs:[si+1],'O'
        jne     rsc_bump
        cmp     byte ptr fs:[si+2],'N'
        jne     rsc_bump
        cmp     byte ptr fs:[si+3],' '
        jne     rsc_bump
        call    rsc_emit
        inc     dx
        cmp     dx,RDBG_SCANN
        jae     rsc_done
rsc_bump:
        inc     si
        jmp     short rsc_look
rsc_next:
        add     bx,0FFDh                ; the stride KRNL386 uses, which
        cmp     bx,0A000h               ; overlaps by 48 bytes so a name on
        jb      rsc_win                  ; a boundary is still seen
rsc_done:
        xor     al,al
        stosb
rsc_out:
        pop     fs
        pop     es
        pop     ds
        popad
        jmp     rr_nostr

;--- one hit, as a space and eight hex digits, at ES:DI --------------
rsc_emit:
        push    eax
        push    ecx
        mov     al,' '
        stosb
        movzx   eax,bx
        shl     eax,4
        movzx   ecx,si
        add     eax,ecx                 ; EAX = linear address of the name
        mov     cx,8
rsc_hex:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     rsc_hex1
        add     al,7
rsc_hex1:
        stosb
        pop     eax
        dec     cx
        jnz     rsc_hex
        pop     ecx
        pop     eax
        ret

rr_nostr:
        call    rdbg_maybe

        pop     ds
        pop     si
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
; rdbg_ret - the OTHER half of a DOS call, which this channel has
;            never recorded.
;
; Everything above logs what was ASKED.  The run that mattered ended
; with Windows' own kernel printing "Incorrect MS-DOS version" after
; an AH=30h, and a trace of the question cannot say whether the answer
; was wrong or whether the answer never came - which is the difference
; between a bug in fn30 and a bug in the path to it.  Called from
; i21_out with BP still on the frame, writing into the slot the entry
; came from.
;---------------------------------------------------------------------
rdbg_ret:
        push    ax
        push    si
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     si,word ptr [rdbg_slot]
        or      si,si
        jz      rt_none
        mov     ax,[bp+F_AX]
        mov     [si+16],ax
        mov     ax,[bp+F_BX]
        mov     [si+18],ax
        mov     ax,[bp+F_CX]
        mov     [si+20],ax
        mov     ax,[bp+F_DX]
        mov     [si+22],ax
        mov     ax,[bp+F_FL]
        mov     [si+24],ax
        mov     word ptr [rdbg_slot],0
        call    rdbg_maybe
rt_none:
        pop     ds
        pop     si
        pop     ax
        ret

;---------------------------------------------------------------------
; The buffer, which IS the disk image of the trace - header first, so
; that the reader on the other end needs to know one offset and not
; two.  Nothing here is initialised at run time; see the head of this
; file for why it does not have to be.
;---------------------------------------------------------------------

;---------------------------------------------------------------------
; rdbg_xin / rdbg_xout - the XMS entry point, which nothing else here
;                        can see.
;
; Windows takes ALL of its extended memory through a far call into the
; driver, not through an interrupt, so the INT 21h trace is blind to
; every one of those calls.  That blindness cost two wrong conclusions:
; that a capped pool was somehow sick, and that WIN.COM's "Insufficient
; extended memory" was not about an amount.  XMSREPT.COM has since shown
; the capped pool is 13.5MB, based at 00283000 and topping out exactly
; at 16MB - perfectly healthy - and that a plain 16MB machine with a
; 3520K pool and no cap at all is refused with the same sentence.
;
; So WIN.COM asks this host something, does not like the answer, and
; gives up.  This records the question and the answer.  Ten bytes a
; call, the FIRST forty, because everything that matters happens before
; WIN.COM has printed a word:
;
;       +0  db  AH  - the function, as asked
;       +1  db  BL  - the error code, as returned
;       +2  dw  DX  - what was asked for (the size, for 09h)
;       +4  dw  AX  - and the three registers a function answers in
;       +6  dw  DX
;       +8  dw  BX
;
; Nothing is written to disk from in here.  A flush is a BIOS disk call
; and the middle of an XMS call - which the extender may be making from
; protected mode - is the worst possible place for one.  The records
; ride out on the next INT 21h, and there is always one: WIN.COM prints
; its complaint through AH=09h before it exits.
;---------------------------------------------------------------------
;---------------------------------------------------------------------
; rdbg_xpool - the pool itself, written into the XMS log as its first
;              record, because the log without it is unreadable.
;
; XMSREPT.COM says a 16MB machine hands RMDOS 3520K based at 00283000.
; The log says WIN.COM asked that same host for 36K and was told there
; is none.  Both cannot be true of the same pool, so the pool is not
; the same pool - and the only way to tell is to have RMDOS state, in
; the trace, what it was given at the moment it was given it.
;
; AH=FFh marks the record as this rather than a call; the reader knows.
;---------------------------------------------------------------------
rdbg_xpool:
        pushad
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     si,word ptr [rdbg_nxms]
        inc     word ptr [rdbg_nxms]
        cmp     si,RDBG_XN
        jae     xp_done
        mov     cx,si
        shl     si,3
        add     si,cx
        add     si,cx                   ; SI = index * RDBG_XL
        add     si,OFFSET rdbg_xms
        mov     byte ptr [si],0FFh
        mov     byte ptr [si+1],0
        mov     eax,dword ptr [rx_base]
        mov     [si+4],ax
        shr     eax,16
        mov     [si+2],ax
        mov     eax,dword ptr [rx_end]
        sub     eax,dword ptr [rx_base]
        shr     eax,10                  ; kilobytes, the unit XMS answers in
        mov     [si+6],ax
        shr     eax,16
        mov     [si+8],ax
        mov     si,word ptr [rdbg_nxms]
        inc     word ptr [rdbg_nxms]
        cmp     si,RDBG_XN
        jae     xp_done
        mov     cx,si
        shl     si,3
        add     si,cx
        add     si,cx
        add     si,OFFSET rdbg_xms
        mov     byte ptr [si],0FEh      ; and what the arena had to give
        mov     byte ptr [si+1],0
        mov     eax,dword ptr [h_rsvd]  ; the owner of the largest block
        mov     [si+4],ax
        shr     eax,16
        mov     [si+2],ax
        mov     eax,dword ptr [h_rsvd+4]
        mov     [si+6],ax               ; its size in KB
        shr     eax,16
        mov     [si+8],ax               ; and who the park ran as
xp_done:
        pop     ds
        popad
        ret

IFDEF XMSPOOLSTAMP
;---------------------------------------------------------------------
; rdbg_xpt - rx_pooltest's answer, as record AH=FDh.
;
; It goes in the XMS log rather than beside the milestones because it
; is a statement about the pool, and the pool record (AH=FFh) it wants
; to be read next to is already there.
;
;   BL     bit 0 - the A20 gate was already open
;          bit 1 - not run: protected mode was not ours to leave
;   +2/+4  the first address that did not read back its own stamp,
;          high word first, or FFFFFFFFh if every one of them did
;   +6/+8  what that address said instead - or, if none did, the
;          number of pages that were asked
;---------------------------------------------------------------------
rdbg_xpt:
        pushad
        push    ds
        mov     ax,cs
        mov     ds,ax
        mov     si,word ptr [rdbg_nxms]
        inc     word ptr [rdbg_nxms]
        cmp     si,RDBG_XN
        jae     xpt_done
        mov     cx,si
        shl     si,3
        add     si,cx
        add     si,cx                   ; SI = index * RDBG_XL
        add     si,OFFSET rdbg_xms
        mov     byte ptr [si],0FDh
        mov     al,byte ptr [rx_ptflg]
        mov     [si+1],al
        mov     eax,dword ptr [rx_ptbad]
        mov     [si+4],ax
        shr     eax,16
        mov     [si+2],ax
        mov     eax,dword ptr [rx_ptval]
        mov     [si+6],ax
        shr     eax,16
        mov     [si+8],ax
xpt_done:
        pop     ds
        popad
        ret
ENDIF

;---------------------------------------------------------------------
; rdbg_env - what the child is actually handed as an environment.
;
; A DOS program is given its environment as a block of NUL-terminated
; strings, an empty string to end them, and then - since DOS 3.0 - the
; word 0001h and the FULL PATH OF THE PROGRAM ITSELF.  That tail is
; not decoration.  KRNL386 walks past the strings, reads the count,
; and OPENS THE PATH IT FINDS THERE to load its own segments; when it
; cannot, it prints "KERNEL: Unable to open KERNEL executable" and
; exits, and no open ever reaches this DOS to say what it wanted.
;
; So the block is recorded from the empty string onwards, tagged FCh,
; and RMTRACE prints it with the NULs shown rather than truncating at
; the first one - the interesting part of this string is all after a
; NUL.  AX = the environment segment, or zero for none.
;---------------------------------------------------------------------
rdbg_env:
        pusha
        push    ds
        push    es
        mov     dx,ax                   ; the environment, before DS goes
        mov     ax,cs
        mov     ds,ax
        mov     es,ax
        mov     bx,word ptr [rdbg_nstr]
        cmp     bx,RDBG_STRN
        jae     re_out
        inc     word ptr [rdbg_nstr]
        mov     ax,bx
        shl     ax,7                    ; * RDBG_STRL
        add     ax,OFFSET rdbg_strs
        mov     di,ax
        mov     al,0FCh
        stosb
        or      dx,dx
        jnz     re_have
        mov     si,OFFSET re_noenv      ; called, and handed nothing
        mov     cx,7
        cld
        rep     movsb
        jmp     short re_out
re_have:
        mov     ds,dx
        xor     si,si
        xor     cx,cx
re_scan:
        cmp     byte ptr [si],0         ; an empty string ends the block
        je      re_found
re_skip:
        inc     si
        inc     cx
        cmp     cx,8192                 ; a runaway block is not a block
        jae     re_found
        cmp     byte ptr [si],0
        jne     re_skip
        inc     si
        inc     cx
        jmp     short re_scan
re_found:
        mov     cx,RDBG_STRL - 1
        cld
        rep     movsb
        jmp     short re_out

re_noenv    db  '<none>',0

re_out:
        pop     es
        pop     ds
        popa
        ret

rdbg_xin:
        push    ds
        push    ax
        mov     ax,cs
        mov     ds,ax
        pop     ax
        mov     byte ptr [rdbg_xah],ah
        mov     word ptr [rdbg_xdx],dx
        pop     ds
        ret

rdbg_xout:
        pushf
        push    bp
        push    ds
        push    si
        push    ax
        push    bx
        push    cx
        push    dx
        mov     si,cs
        mov     ds,si
        mov     bp,sp                   ; the pushed copies, which are the
                                        ; values the caller will SEE - read
                                        ; through SS, written through DS
        mov     si,word ptr [rdbg_nxms]
        inc     word ptr [rdbg_nxms]
        cmp     si,RDBG_XN
        jae     rxo_done
        mov     cx,si
        shl     si,3
        add     si,cx
        add     si,cx                   ; SI = index * RDBG_XL
        add     si,OFFSET rdbg_xms
        mov     al,byte ptr [rdbg_xah]
        mov     [si],al
        mov     ax,word ptr [rdbg_xdx]
        mov     [si+2],ax
        mov     ax,[bp+6]               ; AX returned
        mov     [si+4],ax
        mov     ax,[bp]                 ; DX returned
        mov     [si+6],ax
        mov     ax,[bp+4]               ; BX returned, and BL out of it
        mov     [si+8],ax
        mov     [si+1],al
rxo_done:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        pop     si
        pop     ds
        pop     bp
        popf
        ret

;---------------------------------------------------------------------
; rdbg_hookbios - count the BIOS calls DOSX reflects down to real mode.
;
; KRNL386's boot code, segment 1 at AA56h, is:
;
;       call    B8CC            ; the DOS init - the SFT measurement
;       or      ax,ax
;       jnz     AA62            ; zero means unusable, exit FFh
;   AA62:
;       push    bx
;       mov     bx,[0324h]
;       int     11h             ; the BIOS equipment list
;       ...
;       mov     ax,1600h
;       int     2Fh
;
; The trace's INT 2Fh list is 4300 4310 4A01 1606 and has no 1600h in
; it, so the kernel never reached that line.  The DOS init returned
; success - SFTSCAN says the table is measured correctly - so what is
; left between the two is the INT 11h, which is a BIOS call made from
; protected mode and has to be REFLECTED down by the extender.
;
; That is the question this answers, and it has exactly two answers.
; If the count is non-zero the reflection works and the fault is on
; the way back up.  If it is zero the extender faulted trying to get
; down here at all, and the thing to look at is what PM-DOS left in
; the machine for a mode switch to trip over.
;
; INT 15h is counted too because the extender and HIMEM live on it and
; a zero there would say the same thing about a call that happens much
; earlier.  Neither hook does any work: they count and chain, because
; a flush from inside an interrupt the extender is in the middle of
; reflecting is a disk write in the worst possible place.  The counts
; ride out on the next INT 21h, and there is always one - the kernel
; prints its complaint through AH=09h before it exits.
;---------------------------------------------------------------------
rdbg_hookbios:
        push    ds
        push    ax
        push    di
        xor     ax,ax
        mov     ds,ax

        mov     di,11h * 4
        cli
        mov     ax,word ptr ds:[di]
        mov     word ptr cs:[rdbg_i11old],ax
        mov     ax,word ptr ds:[di+2]
        mov     word ptr cs:[rdbg_i11old+2],ax
        mov     word ptr ds:[di],OFFSET i11dbg
        mov     ax,cs
        mov     word ptr ds:[di+2],ax

        mov     di,15h * 4
        mov     ax,word ptr ds:[di]
        mov     word ptr cs:[rdbg_i15old],ax
        mov     ax,word ptr ds:[di+2]
        mov     word ptr cs:[rdbg_i15old+2],ax
        mov     word ptr ds:[di],OFFSET i15dbg
        mov     ax,cs
        mov     word ptr ds:[di+2],ax
        sti

        pop     di
        pop     ax
        pop     ds
        ret

i11dbg:
        inc     word ptr cs:[rdbg_n11]
        cmp     word ptr cs:[rdbg_n11],1
        jne     i11go                   ; only the first is timestamped
        push    bp
        push    ax
        mov     bp,sp                   ; [bp+4] IP, [bp+6] CS, [bp+8] flags
        mov     ax,word ptr cs:[rdbg_count]
        mov     word ptr cs:[rdbg_i11at],ax
        pop     ax
        pop     bp
i11go:
        jmp     dword ptr cs:[rdbg_i11old]

i15dbg:
        inc     word ptr cs:[rdbg_n15]
        jmp     dword ptr cs:[rdbg_i15old]

rdbg_i11old dd  0
rdbg_i15old dd  0

rdbg_buf:
rdbg_sig    db  'RMDBG',0,0,0           ; +00
rdbg_count  dw  0                       ; +08  INT 21h calls served, total
rdbg_head   dw  0                       ; +0A  the next slot to write
rdbg_marks  dw  0                       ; +0C  RDBGM_* bits
rdbg_2fseen    dw  0                       ; +0E  INT 2Fh calls seen
rdbg_frame0 db  RMI_LEN dup (0)         ; +10  the frame AS IT ARRIVED
rdbg_frame  db  RMI_LEN dup (0)         ; +40  ...and as it is when we use it
rdbg_pseg   dw  0                       ; +70  where this DOS parked
rdbg_pbnc   dw  0                       ; +72  the bounce buffer segment
rdbg_n11    dw  0                       ; +74  INT 11h reflected down
rdbg_n15    dw  0                       ; +76  INT 15h reflected down
rdbg_i11at  dw  0                       ; +78  INT 21h count when 11h came
rdbg_cpsp   dw  0                       ; +7A  the PSP RMDOS built for the
rdbg_ctop   dw  0                       ; +7C  last child, and its PSP:2 -
                                        ; the top of memory the child is
                                        ; told it owns
rdbg_nxms   dw  0                       ; +7E  XMS calls served, total
;--- +80: the handle table, which is where v0.55 went looking ---------
; "Too many open files" is a statement about a table, and there are
; two of them: the per-process one in the PSP, which DOS builds with
; twenty entries and 67h moves and grows, and RMDOS's own SFT behind
; it.  A refusal names neither.  These five words say which PSP was
; current, what its table looks like, how many of its entries are in
; use at the moment of the flush, and whether anybody ever asked 67h
; for a bigger one.
rdbg_n67    dw  0                       ; +80  AH=67h calls seen
rdbg_h67a   dw  0                       ; +82  the first count asked for
rdbg_h67b   dw  0                       ; +84  ...and the last
rdbg_lpsp   dw  0                       ; +86  the PSP at the last flush
rdbg_jcnt   dw  0                       ; +88  how many entries it has
rdbg_jseg   dw  0                       ; +8A  and where they are
rdbg_joff   dw  0                       ; +8C
rdbg_jused  dw  0                       ; +8E  how many are in use
rdbg_nfail  dw  0                       ; +90  opens refused for a handle
rdbg_fpsp   dw  0                       ; +92  the PSP the FIRST one hit
rdbg_fcnt   dw  0                       ; +94  its table size
rdbg_fused  dw  0                       ; +96  and how much of it was used
rdbg_hpad   db  8 dup (0)               ; +98  to the end of the header
rdbg_2fax   dw  RDBG_2FN dup (0)        ; +80  the first few of their AX
rdbg_strs   db  RDBG_STRN * RDBG_STRL dup (0)   ; +C0  what it PRINTED
rdbg_xms    db  RDBG_XN * RDBG_XL dup (0)       ; and what it asked the XMS
                                                ; host, which no other channel
                                                ; here can see
rdbg_lstrs  db  RDBG_LSTN * RDBG_STRL dup (0)   ; and the last names it opened
rdbg_ents   db  (RDBG_LEN - RDBG_EOFF) dup (0)
