;=====================================================================
; RMDOS.ASM - RMDOS.SYS, the real-mode DOS PM-DOS leaves behind
;
; PM-DOS is a protected-mode kernel whose entire API is 32-bit code.
; Windows in 386 enhanced mode needs the machine's supervisor seat for
; itself, so the only way to run it is for PM-DOS to GET OUT - drop
; paging, leave protected mode, and hand the machine over.  The moment
; it does, every INT 21h in the machine has nothing to answer it.
;
; This is what answers it.  A second, smaller DOS, 16-bit and real
; mode throughout, loaded into the arena just before the handover and
; freed after it, whose only job is to be underneath Windows.
;
; See INC\RMDOS.INC for the contract, and for why a stub that merely
; FORWARDED calls to PM-DOS is not possible: while WIN386 owns CR0,
; CR3, the GDT and the IDT there is no way back into PM-DOS's own
; protected mode, so this has to implement DOS rather than relay it.
;
;---------------------------------------------------------------------
; WHAT IT DOES NOT HAVE TO DO, which is most of why it fits.
;
; The arena is already a real-mode MCB chain in real conventional
; memory, because that is the only shape a 16-bit program can walk;
; the PSPs in it are real PSPs; the programs are at real segment
; addresses; and below 1MB PM-DOS's page map is the identity, so when
; paging stops every one of those addresses still means what it meant.
; RMDOS adopts all of it.  It allocates nothing, moves nothing and
; relocates nothing at startup.
;
; What it has to supply is what lived above 1MB and did not survive:
; the handle table, the file system, and the INT 21h that reaches them.
;
;---------------------------------------------------------------------
; ONE SEGMENT, AND CS = DS = SS THROUGHOUT.  Code, data and all three
; internal stacks are in the same 64K, which is why nothing in this
; file needs a segment override to reach its own variables and why the
; register frame can be addressed as [bp+F_xx] with the processor's
; default SS and read back as ordinary data.  The caller's segments
; are the only ones that need naming, and they are always named.
;
; Linked with:  wlink form raw bin option offset=0
;=====================================================================
        .386p                   ; p, for the LGDT and CR0 the block
                                ; move needs - see RMXMS.INC
        INCLUDE RMDOS.INC

;---------------------------------------------------------------------
; The INT 21h register frame, as this file builds it: PUSHA, then DS
; and ES, with BP pointing at the lot.  PUSHA stores DI last and so
; lowest, which is why the order below looks upside down.  The three
; words above it are the ones the INT itself pushed.
;---------------------------------------------------------------------
F_ES        EQU 00h
F_DS        EQU 02h
F_DI        EQU 04h
F_SI        EQU 06h
F_BP        EQU 08h
F_SP        EQU 0Ah
F_BX        EQU 0Ch
F_DX        EQU 0Eh
F_CX        EQU 10h
F_AX        EQU 12h
F_IP        EQU 14h
F_CS        EQU 16h
F_FL        EQU 18h
F_LEN       EQU 1Ah                     ; 13 words, and all of them travel

; ...and the two words pushed UNDER it, which say where the caller's
; own copy is so the answers can be written back into it.
F_OWNOFF    EQU F_LEN + 00h
F_OWNSEG    EQU F_LEN + 02h

FL_CF       EQU 0001h
FL_ZF       EQU 0040h

RD_STKSIZE  EQU 1024
RD_STKLVL   EQU 3
I21_MAX     EQU 6Dh

_RD     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RD, DS:_RD, ES:NOTHING, SS:_RD

; NOT ZERO.  Sixteen, so that segment FFFFh - the only segment that
; reaches above the megabyte - puts this byte at exactly 100000h and
; the image sits in the HMA.  See the note above RM_ORG in RMDOS.INC;
; every RMH_* offset carries it, so nothing below moves.
        ORG     RM_ORG

rd_base:
        jmp     rd_entry                ; RMH_JMP - three bytes, E9 + word

;---------------------------------------------------------------------
; The handoff block.  PM-DOS fills this IN THE IMAGE before it hands
; over, because between the fill and the entry there is a processor
; mode switch and the only thing certain to come through it is memory.
;---------------------------------------------------------------------
        ORG     RMH_BASE
h_sig       dd  0
h_ver       dw  0
h_unpseg    dw  0
h_unpoff    dw  0
h_mcbhead   dw  0
h_psp       dw  0
h_bootdrv   db  0
h_curdrv    db  0
h_ndrv      db  0
h_flags     db  0
h_runseg    dw  0
h_runoff    dw  0
h_runsp     dw  0
h_runss     dw  0
h_memtop    dw  0
h_exitcode  dw  0
h_why       dw  0
h_xmsbase   dd  0
h_xmslen    dd  0
h_rsvd      dd  0, 0

        ORG     RMH_IRET
h_frame     db  RMI_LEN dup(0)

        ORG     RMH_DRIVES
h_drives    db  (LASTDRIVE * RMD_LEN) dup(0)

;=====================================================================
; Code proper.  Everything above is a header PM-DOS writes into.
;=====================================================================
        ORG     RM_ORG + 0100h

;=====================================================================
; rd_entry - the first instruction executed in real mode after the
;            handover.  CS = our segment and nothing else is promised.
;=====================================================================
rd_entry:
        cli
IFDEF RMDOS_MARK
;--- THE ONLY INSTRUMENT AVAILABLE TO CODE THAT MAY NOT HAVE ARRIVED.
;--- No BIOS, no DOS, no initialised anything - a word into the text
;--- page, which is at B800:0 whatever else is or is not true.  The
;--- markers run 1 to 5 across the top right of the screen and between
;--- them they say which side of the far jump the machine stopped on.
        mov     al,'2'
        call    rd_vmark
ENDIF
        mov     ax,cs
        mov     ds,ax
        mov     es,ax
        mov     ss,ax
        mov     sp,OFFSET rd_stktop

        cmp     dword ptr [h_sig],RMDOS_SIG
        je      rd_sigok

        ; No signature means PM-DOS did not fill the header, which
        ; means this image is not the one it thinks it loaded.  There
        ; is nowhere to report that from - the screen belongs to
        ; whatever ran last - so say it where the other side will look,
        ; and go straight back.
        mov     word ptr [h_why],RMW_NOTOURS
        jmp     rd_goback
rd_sigok:
IFDEF RMDOS_MARK
        mov     al,'3'
        call    rd_vmark
ENDIF
        mov     word ptr [h_why],RMW_EXIT
IFDEF DBG_LPT
        call    rdbg_init               ; somewhere low to write from - see
        call    rdbg_saveframe0         ; RMDBG.INC, and v0.51
        mov     al,RDBGM_ENTRY
        call    rdbg_mark
ENDIF
IFDEF RMDOS_BISECT
        jmp     rd_goback               ; straight back out: does the
                                        ; SWITCH work, before asking
                                        ; whether the DOS does
ENDIF

        call    rd_vecs                 ; the vectors a DOS owns
IFDEF RMDOS_MARK
        mov     al,'4'
        call    rd_vmark
ENDIF
IFDEF DBG_LPT
        mov     al,RDBGM_VECS
        call    rdbg_mark
        call    rdbg_hookbios           ; count what the extender reflects
ENDIF
        call    rd_volinit              ; per-drive geometry
IFDEF RMDOS_MARK
        mov     al,'5'
        call    rd_vmark
ENDIF
IFDEF DBG_LPT
        mov     al,RDBGM_VOL
        call    rdbg_mark
ENDIF
        call    rd_sysinit              ; SDA, list of lists, CDS
IFDEF RMDOS_MARK
        mov     al,'6'
        call    rd_vmark
ENDIF
IFDEF DBG_LPT
        mov     al,RDBGM_SYS
        call    rdbg_mark
ENDIF
        call    rd_sftinit              ; the handle table and its devices
IFDEF RMDOS_MARK
        mov     al,'7'
        call    rd_vmark
ENDIF
IFDEF DBG_LPT
        mov     al,RDBGM_SFT
        call    rdbg_mark
ENDIF
        call    rx_init                 ; extended memory, and the A20 gate
IFDEF RMDOS_MARK
        mov     al,'8'
        call    rd_vmark
ENDIF
IFDEF DBG_LPT
        mov     al,RDBGM_XMS
        call    rdbg_mark
ENDIF

        mov     ax,[h_psp]
        mov     [rd_psp],ax
        mov     [rd_dtaseg],ax
        mov     al,[h_curdrv]
        mov     [rd_drive],al

        test    byte ptr [h_flags],RMHF_VERBOSE
        jz      rd_quiet
        mov     si,OFFSET msg_hello
        call    rd_say
rd_quiet:
        mov     ax,[h_runseg]
        or      ax,ax
        jz      rd_resume

;---------------------------------------------------------------------
; Start a program at RMH_RUNSEG:RMH_RUNOFF.
;
; This is how RMDOS is tested without Windows anywhere in the picture:
; PM-DOS loads an ordinary 16-bit program, parks, and this starts it.
; The program's 4Ch comes back through fn4C to rd_goback, so one
; excursion is one program.  A real handover takes the other branch.
;---------------------------------------------------------------------
IFDEF DBG_LPT
        mov     al,RDBGM_RUN
        call    rdbg_mark
ENDIF
        mov     ax,[h_runoff]
        mov     word ptr [rd_go + 1],ax
        mov     ax,[h_runseg]
        mov     word ptr [rd_go + 3],ax

        mov     ax,[h_runss]
        mov     bx,[h_runsp]
        cli
        mov     ss,ax
        mov     sp,bx
        mov     ax,[h_psp]
        mov     ds,ax
        mov     es,ax
        xor     ax,ax
        sti
rd_go:
        db      0EAh                    ; JMP FAR <patched>:<patched>
        dw      0
        dw      0

;---------------------------------------------------------------------
; THE HANDOVER ITSELF.  WIN386 is already loaded and already running
; when it broadcasts INT 2Fh AX=1605h; there is nothing to start.  What
; it is owed is a return from that interrupt - and the return happens
; to land in a different processor mode from the one the interrupt was
; taken in, which is a thing only the machine underneath can arrange
; and which the program above cannot detect.
;---------------------------------------------------------------------
rd_resume:
IFDEF RMDOS_MARK
        mov     al,'9'                  ; every part of RMDOS is up, and
        call    rd_vmark                ; the next instruction is Windows'
ENDIF
IFDEF DBG_LPT
        call    rdbg_saveframe
        mov     al,RDBGM_RESUME
        call    rdbg_mark
ENDIF
        cli
        mov     ax,word ptr [h_frame + RMI_SS - RMH_IRET]
        mov     bx,word ptr [h_frame + RMI_ESP - RMH_IRET]
        mov     ss,ax
        mov     sp,bx

        mov     ax,word ptr [h_frame + RMI_FLAGS - RMH_IRET]     ; an IRET frame on
        push    ax                                      ; the caller's own
        mov     ax,word ptr [h_frame + RMI_CS - RMH_IRET]        ; stack, where it
        push    ax                                      ; would have been
        mov     ax,word ptr [h_frame + RMI_IP - RMH_IRET]
        push    ax

        mov     eax,dword ptr [h_frame + RMI_EBX - RMH_IRET]
        mov     ebx,eax
        mov     eax,dword ptr [h_frame + RMI_ECX - RMH_IRET]
        mov     ecx,eax
        mov     eax,dword ptr [h_frame + RMI_EDX - RMH_IRET]
        mov     edx,eax
        mov     eax,dword ptr [h_frame + RMI_ESI - RMH_IRET]
        mov     esi,eax
        mov     eax,dword ptr [h_frame + RMI_EDI - RMH_IRET]
        mov     edi,eax
        mov     eax,dword ptr [h_frame + RMI_EBP - RMH_IRET]
        mov     ebp,eax
        mov     ax,word ptr [h_frame + RMI_ES - RMH_IRET]
        mov     es,ax
        mov     ax,word ptr [h_frame + RMI_DS - RMH_IRET]
        push    ax
        mov     eax,dword ptr [h_frame + RMI_EAX - RMH_IRET]
        pop     ds
        iret

;=====================================================================
; rd_goback - unpark.  The excursion is over; give the machine back.
;
; FAR JMP to an address we were HANDED rather than one compiled in,
; because PMIO may be anywhere and RMDOS.SYS is built separately.
; Interrupts stay off from here: the IVT is about to belong to a
; kernel that is not running yet.
;=====================================================================
rd_goback:
        cli
IFDEF RMDOS_MARK
        mov     al,'4'
        call    rd_vmark
ENDIF
IFDEF DBG_LPT
        mov     al,RDBGM_BACK
        call    rdbg_mark
ENDIF
        mov     ax,cs
        mov     ds,ax
        mov     ss,ax
        mov     sp,OFFSET rd_stktop
        call    rx_done                 ; the vector we took off the BIOS
        mov     ax,[rd_exitcode]
        mov     [h_exitcode],ax
        mov     ax,[h_unpoff]
        mov     word ptr [rd_back + 1],ax
        mov     ax,[h_unpseg]
        mov     word ptr [rd_back + 3],ax
IFDEF RMDOS_FREEZE
        ; STOP HERE INSTEAD, so that whatever the excursion left on the
        ; screen stays there.  The unpark resets this machine, and a
        ; standard-mode fault report survives about half a second: a
        ; once-a-second screen poll caught it ONCE in twelve boot loops,
        ; which is not an instrument, it is luck.  Everything above has
        ; already run, so the trace is written and the vectors are back;
        ; only the jump home is missing, and the machine that is not
        ; going home does not need it.
IFDEF XMSPOOLSTAMP
        ; And with the stamps in place, say what the excursion wrote:
        ; the count of pages that lost theirs, the first and the last.
        ; See rx_poolscan in RMXMS.INC.
        call    rx_poolscan
        call    rx_psshow
ENDIF
rd_freeze:
        cli
        hlt
        jmp     short rd_freeze
ENDIF
rd_back:
        db      0EAh                    ; JMP FAR <patched>:<patched>
        dw      0
        dw      0

IFDEF RMDOS_MARK
;--- AL = a character, onto the screen and nowhere else.  CS overrides
;--- throughout, because DS is the caller's and the caller may be four
;--- instructions old.
;
;--- BOTH PAGES, BECAUSE THE MODE IS NOT OURS TO KNOW.  PARKWIN leaves
;--- the machine in text mode and the character lands at B800:xx;
;--- Windows has already switched to 640x480 and B800 is not on the
;--- screen at all, so the same marker also goes to A000:xx, where in
;--- a planar mode it paints eight pixels.  Neither write can fault
;--- and the wrong one is invisible rather than harmful.  A marker
;--- that only shows up in the mode you were not debugging is no
;--- marker: v0.48 lost a week to a prompt drawn where nobody could
;--- see it.
rd_vmark:
        push    ax
        push    bx
        push    ds
        mov     ah,1Fh
        mov     bx,word ptr cs:[rd_vcol]
        add     word ptr cs:[rd_vcol],2
        push    ax
        mov     ax,0B800h
        mov     ds,ax
        pop     ax
        mov     word ptr [bx],ax
        push    ax
        mov     ax,0A000h               ; ...and again where Windows is
        mov     ds,ax
        pop     ax
        shl     bx,3                    ; eight pixels per marker, spread
        mov     word ptr [bx],0FFFFh    ; far enough apart to count
        pop     ds
        pop     bx
        pop     ax
        ret
;=====================================================================
; rd_vbars / rd_vbarout - three bars in the graphics framebuffer, and
; the only instrument that works here.
;
; The disk sink does not: INT 13h WRITES from the HMA do nothing on
; this BIOS, though reads into it are fine, so RMDBG.INC's channel
; went silent the moment RMDOS moved up here.  The screen is the one
; thing both sides of the handover agree about, and Windows has it in
; 640x480, so these are painted as PIXELS - a bar AH long, which a
; script can measure off a screenshot without knowing a font.
;
;   y = 400   AH of the DOS call going IN
;   y = 410   AH of the last one that came back OUT
;   y = 420   how many calls have been served, modulo 256
;
; Equal top two bars and a moving third: Windows is running and
; talking to DOS.  Top bar alone: it went into that call and did not
; come out.  All three frozen: it has stopped asking DOS anything,
; and the fault is not in here.
;=====================================================================
RD_VBROW    EQU 460 * 80          ; below the logo box, which is not
RD_VBOUT    EQU 465 * 80          ; where the first attempt put them
RD_VBCNT    EQU 470 * 80

rd_vbars:
        push    ax
        push    cx
        push    di
        push    es
        mov     ax,0A000h
        mov     es,ax
        mov     al,[bp+F_AX+1]          ; AH, out of the frame
        mov     di,RD_VBROW
        call    rd_vbar
        inc     byte ptr cs:[rd_vbn]
        mov     al,byte ptr cs:[rd_vbn]
        mov     di,RD_VBCNT
        call    rd_vbar
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

rd_vbarout:
        push    ax
        push    cx
        push    di
        push    es
        mov     ax,0A000h
        mov     es,ax
        mov     al,[bp+F_AX+1]
        mov     di,RD_VBOUT
        call    rd_vbar
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

;--- AL = the length in bytes, ES:DI = the start of the row.  The row
;--- is cleared first, or a shorter bar reads as the longer one before
;--- it - which is the same mistake as a trace that is not cleared.
rd_vbar:
        push    ax
        push    di
        mov     cx,320                  ; four rows: a bar can be 255 bytes
        push    ax
        xor     al,al
        cld
        rep     stosb
        pop     ax
        pop     di
        movzx   cx,al
        or      cx,cx
        jz      rvb_none
        mov     al,0FFh
        rep     stosb
rvb_none:
        pop     ax
        ret

rd_vbn      db  0
rd_vstg     db  0                  ; the stages so far, one bit each

;--- AL = which stage of EXEC, onto a row of its own.  EXEC is the
;--- one call that never returns through i21_out, so the two bars
;--- above cannot say anything about what happens inside it.
RD_VBSTG    EQU 448 * 80
;--- THE ROWS ABOVE BELONG TO THE IN/OUT BARS AND TO NOTHING ELSE.
;--- A probe that borrowed them to show the child's entry bytes read
;--- 4D 5A - 'MZ' - which was the finding that mattered, and then went
;--- on reading 4D 5A on a run where it had never executed, because
;--- those are also plausible values for the bars that own the rows.
;--- One row, one meaning.
; AL = ONE BIT, not a number.  Stages that overwrite each other say
; only where the code got to last, and the branch taken three stages
; back is exactly what was wanted.  So they accumulate: 3Bh means the
; .EXE path all the way to the jump, 3Dh means the .COM path did.
rd_vstage:
        push    ax
        push    cx
        push    di
        push    es
        or      byte ptr cs:[rd_vstg],al
        mov     al,byte ptr cs:[rd_vstg]
        mov     cx,0A000h
        mov     es,cx
        mov     di,RD_VBSTG
        call    rd_vbar
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret


;--- AX as four hex digits, through the same channel ----------------
rd_vhex:
        push    ax
        push    cx
        mov     cx,4
rvh_one:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     rvh_dig
        add     al,'A' - '0' - 10
rvh_dig:
        call    rd_vmark
        pop     ax
        loop    rvh_one
        pop     cx
        pop     ax
        ret

rd_vcol     dw  40                      ; row 0, column 20 - the only row
                                        ; PM-DOS's own marker
ENDIF

;=====================================================================
; rd_vecs - take the vectors a DOS owns.
;
; Every one is set unconditionally.  Whatever was there a moment ago
; was PM-DOS's V86 monitor reflecting into a guest, and there is no
; monitor now - so there is nothing to chain to and nothing to save.
;=====================================================================
rd_vecs:
        push    es
        xor     ax,ax
        mov     es,ax
        mov     si,OFFSET rd_vectab
rv_loop:
        lodsb
        or      al,al
        jz      rv_done
        movzx   di,al
        shl     di,2
        lodsw
        cli
        mov     word ptr es:[di],ax
        mov     ax,cs
        mov     word ptr es:[di+2],ax
        sti
        jmp     short rv_loop
rv_done:
        pop     es
        ret

rd_vectab:
        db  20h
        dw  OFFSET i20
        db  21h
        dw  OFFSET i21
        db  22h
        dw  OFFSET i_iret
        db  23h
        dw  OFFSET i23
        db  24h
        dw  OFFSET i24
        db  25h
        dw  OFFSET i25
        db  26h
        dw  OFFSET i26
        db  27h
        dw  OFFSET i27
        db  28h
        dw  OFFSET i_iret
        db  29h
        dw  OFFSET i29
        db  2Ah
        dw  OFFSET i_iret
        db  2Bh
        dw  OFFSET i_iret
        db  2Ch
        dw  OFFSET i_iret
        db  2Dh
        dw  OFFSET i_iret
        db  2Eh
        dw  OFFSET i_iret
        db  2Fh
        dw  OFFSET i2F
        db  0

;=====================================================================
; The small vectors
;=====================================================================
i_iret:
        iret

;--- 20h - terminate, the DOS 1 spelling.  CS is the PSP -------------
i20:
        mov     ax,4C00h
        int     21h

;--- 27h - terminate and stay resident, the DOS 1 spelling -----------
; DX = bytes to keep, CS = PSP.  Turned into 31h's paragraphs, and the
; carry out of the rounding add is the seventeenth bit of the answer.
i27:
        add     dx,15
        rcr     dx,1
        shr     dx,3
        and     dx,1FFFh
        mov     ax,3100h
        int     21h

;--- 23h - the default Ctrl-C handler, which is to end the program ---
i23:
        mov     ax,4C00h
        int     21h

;--- 29h - fast console write.  AL = the character -------------------
i29:
        push    ax
        push    bx
        push    dx
        mov     dl,al
        call    con_out
        pop     dx
        pop     bx
        pop     ax
        iret

;=====================================================================
; INT 21h
;
; THE STACK IS SWITCHED AND THE FRAME TRAVELS WITH IT.  A .COM with no
; stack of its own gets a couple of hundred bytes at the top of its own
; segment, and DOS has always moved off it before doing any work.  The
; frame is COPIED onto ours rather than left behind, so every handler
; can reach it as [bp+F_xx] with the processor's default SS and read it
; as ordinary data - CS, DS and SS are all this segment.  Two words
; underneath say where the caller's copy is, and i21_out writes the
; answers back into it.
;
; Three levels, and the third is not generosity: a critical-error
; handler may call DOS, and the call it makes may raise one.
;=====================================================================
IFDEF RMDOS_EDBG
;--- DECLARED HERE, AHEAD OF ITS FIRST USE, AND THAT IS THE POINT.
;--- These two bytes are read by the marker below and written by fn4B
;--- in RMMEM.INC, which this file INCLUDEs further down - so wherever
;--- they are declared, one of the two references is a forward one.
;--- Put them last and the reference that goes forward is the one in
;--- the instrument, which then cannot be trusted about anything: an
;--- instrument that may be reading the wrong byte reports a silence
;--- for "no" and for "I could not tell", and those are the two
;--- answers it exists to separate.
ed_post     db  0                   ; 1 once EXEC has jumped: from then
ed_left     db  8                   ; on, every call is the child's
ed_sz       dd  0                   ; what sub_read's SFT says the file is
ed_pos      dd  0                   ; ...and where in it the read starts
ENDIF

i21:
        cli
        pusha
        push    ds
        push    es
IFDEF RMDOS_EDBG
        ; THE FIRST THING AFTER THE PUSHES, and it needs nothing that
        ; has not already been saved.  ed_i21 below is a long way in -
        ; past the stack switch and the frame copy - so its silence
        ; cannot say whether the call ARRIVED.  This can: an 'A' on the
        ; bottom line means an INT 21h got through the vector and onto
        ; a stack that could take nine pushes.  ed_post gates it, so
        ; only calls made after EXEC has jumped are counted.
        cmp     byte ptr cs:[ed_post],0
        je      i21_nomarka
        mov     ax,0B800h
        mov     es,ax
        mov     word ptr es:[24*160 + 20],0F41h ; 'A'
i21_nomarka:
ENDIF
        mov     bp,sp
        mov     cx,ss                   ; where the caller's frame is
        mov     dx,bp

        mov     ax,cs
        mov     ds,ax
        inc     byte ptr [rd_indos]

        ; THE CALLER'S STACK, IN THE CALLER'S OWN PSP.  DOS writes
        ; SS:SP into PSP+2Eh on every single call - msdisp.asm does it
        ; three instructions after raising InDOS - and the only code
        ; that ever reads it back is process termination: the parent is
        ; resumed on the stack it was standing on when it made the call
        ; that started the child.
        ;
        ; RMDOS kept ONE copy of that, written by 4Bh.  Windows does
        ; not EXEC its tasks - it builds a PSP for each of them with
        ; 55h and starts them itself - so when the first one exited,
        ; the single copy still described WIN.COM's EXEC of DOSX and
        ; RMDOS returned there.  Pressing Escape on the Task List
        ; ended the whole Windows session.
        push    es
        mov     es,[rd_psp]
        mov     word ptr es:[2Eh],dx
        mov     word ptr es:[30h],cx
        pop     es

        mov     al,[rd_stkdep]
        push    ax                      ; the caller's PSP, at this level
        push    bx
        movzx   bx,al
        cmp     bx,RD_STKLVL
        jae     i21_nopsp
        shl     bx,1
        mov     ax,[rd_psp]
        mov     [rd_stkpsp + bx],ax
i21_nopsp:
        pop     bx
        pop     ax
        inc     byte ptr [rd_stkdep]
        cmp     al,RD_STKLVL
        jae     i21_stacked             ; no level left: stay where we are,
                                        ; which is what DOS did too
        movzx   bx,al
        shl     bx,1
        mov     ax,[rd_stktab + bx]
        mov     bx,cs
        mov     ss,bx
        mov     sp,ax
i21_stacked:
        push    cx                      ; F_OWNSEG
        push    dx                      ; F_OWNOFF

        mov     es,cx                   ; copy the frame across, top word
        mov     si,dx                   ; first, so the copy lands the same
        add     si,F_LEN                ; way up as the original
        mov     cx,F_LEN / 2
i21_cpy:
        sub     si,2
        push    word ptr es:[si]
        loop    i21_cpy
        mov     bp,sp


        mov     ax,cs
        mov     es,ax
        sti
        cld

IFDEF RMDOS_MARK
        call    rd_vbars                ; the last call in, and the count
ENDIF
IFDEF RMDOS_EDBG
        call    ed_i21                  ; ...and what arrives after EXEC
ENDIF
IFDEF DBG_LPT
        call    rdbg_rec                ; BP is the frame - see RMDBG.INC
ENDIF

        mov     bl,byte ptr [bp+F_AX+1]
        movzx   bx,bl
        cmp     bx,I21_MAX
        jae     i21_bad
        shl     bx,1
        call    word ptr [i21_tab + bx]
        jmp     short i21_out
i21_bad:
        call    i21_unsup

i21_out:
        cli
IFDEF DBG_LPT
        call    rdbg_ret                ; what the call ANSWERED
ENDIF
IFDEF RMDOS_MARK
        call    rd_vbarout              ; ...and which call came back
ENDIF
        mov     si,bp                   ; our copy...
        mov     di,[bp+F_OWNOFF]        ; ...back into theirs
        mov     es,[bp+F_OWNSEG]
        mov     cx,F_LEN / 2
        cld
        rep     movsw

        mov     ax,[bp+F_OWNSEG]
        mov     bx,[bp+F_OWNOFF]
        dec     byte ptr [rd_stkdep]
        dec     byte ptr [rd_indos]
        mov     ss,ax
        mov     sp,bx
        pop     es
        pop     ds
        popa
        iret

;---------------------------------------------------------------------
; The answer helpers.  Every handler ends through one of these, and
; they are the only places that touch the caller's flags.
;---------------------------------------------------------------------
i21_ok:
        and     word ptr [bp+F_FL],NOT FL_CF
        ret

i21_fail:                               ; AX = the error code
        mov     [bp+F_AX],ax
        mov     [rd_lasterr],ax
        or      word ptr [bp+F_FL],FL_CF
        ret

i21_unsup:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; The dispatch table.  Anything not here is refused rather than
; ignored: a program told "no" can say which call failed, where one
; given a silent success cannot.
;=====================================================================
i21_tab:
        dw  fn00,      fn01,      fn02,      i21_unsup   ; 00-03
        dw  i21_unsup, i21_unsup, fn06,      fn07        ; 04-07
        dw  fn08,      fn09,      fn0A,      fn0B        ; 08-0B
        dw  fn0C,      fn0D,      fn0E,      i21_unsup   ; 0C-0F
        dw  i21_unsup, i21_unsup, i21_unsup, i21_unsup   ; 10-13
        dw  i21_unsup, i21_unsup, i21_unsup, i21_unsup   ; 14-17
        dw  i21_unsup, fn19,      fn1A,      i21_unsup   ; 18-1B
        dw  i21_unsup, i21_unsup, i21_unsup, i21_unsup   ; 1C-1F
        dw  i21_unsup, i21_unsup, i21_unsup, i21_unsup   ; 20-23
        dw  i21_unsup, fn25,      fn26,      i21_unsup   ; 24-27
        dw  i21_unsup, i21_unsup, fn2A,      fn2B        ; 28-2B
        dw  fn2C,      fn2D,      fn2E,      fn2F        ; 2C-2F
        dw  fn30,      fn31,      i21_unsup, fn33        ; 30-33
        dw  fn34,      fn35,      fn36,      fn37        ; 34-37
        dw  fn38,      fn39,      fn3A,      fn3B        ; 38-3B
        dw  fn3C,      fn3D,      fn3E,      fn3F        ; 3C-3F
        dw  fn40,      fn41,      fn42,      fn43        ; 40-43
        dw  fn44,      fn45,      fn46,      fn47        ; 44-47
        dw  fn48,      fn49,      fn4A,      fn4B        ; 48-4B
        dw  fn4C,      fn4D,      fn4E,      fn4F        ; 4C-4F
        dw  fn50,      fn51,      fn52,      i21_unsup   ; 50-53
        dw  fn54,      fn55,      fn56,      fn57        ; 54-57
        dw  fn58,      fn59,      fn5A,      fn5B        ; 58-5B
        dw  fn5C,      fn5D,      i21_unsup, i21_unsup   ; 5C-5F
        dw  i21_unsup, i21_unsup, fn51,      fn63        ; 60-63
        dw  i21_unsup, fn65,      i21_unsup, fn67        ; 64-67
        dw  fn68,      i21_unsup, fn68,      i21_unsup   ; 68-6B
        dw  fn6C                                         ; 6C

IF ($ - i21_tab) NE I21_MAX * 2
        .ERR    <i21_tab does not cover 00h..I21_MAX-1>
ENDIF

;---------------------------------------------------------------------
; rd_say - DS:SI -> ASCIZ, straight to the BIOS.
;
; Deliberately not through the console path below: this is used before
; that path exists and while reporting that it does not.
;---------------------------------------------------------------------
rd_say:
        push    ax
        push    bx
        push    si
rs_loop:
        lodsb
        or      al,al
        jz      rs_done
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        jmp     short rs_loop
rs_done:
        pop     si
        pop     bx
        pop     ax
        ret

        INCLUDE RMCON.INC               ; console, and the character calls
        INCLUDE RMDISK.INC              ; sectors, BPBs, INT 25h/26h
        INCLUDE RMFILE.INC              ; FAT, directories, handles
        INCLUDE RMFILE2.INC             ; ...and the handles and calls
        INCLUDE RMMEM.INC               ; the arena, EXEC and terminate
        INCLUDE RMINFO.INC              ; everything that only answers
        INCLUDE RMXMS.INC               ; extended memory, A20 and INT 15h
IFDEF DBG_LPT
        INCLUDE RMDBG.INC               ; and, when the build asks for it, a
                                        ; trace that survives a hang - which
                                        ; is the only kind that is any use on
                                        ; this side of the handover
ENDIF

;=====================================================================
; Data
;=====================================================================
rd_indos    db  0                       ; published through 34h and the SDA
rd_errmode  db  0
rd_stkdep   db  0
rd_psp      dw  0
rd_drive    db  0
rd_lasterr  dw  0
rd_verify   db  0
rd_brkchk   db  0
rd_exitcode dw  0
rd_exitway  db  0
rd_dtaseg   dw  0
rd_dtaoff   dw  80h

msg_hello   db  13,10,'RMDOS is the DOS in this machine now.',13,10,0

rd_stktab   dw  OFFSET rd_stk1, OFFSET rd_stk2, OFFSET rd_stk3

;--- WHO IS WAITING IN WHICH CALL.  One PSP per nesting level, written
;--- at i21 entry and read by term_common: a process that ends has to
;--- know whether the parent it names is standing inside a DOS call -
;--- an EXEC that has not returned - or is simply the PSP written at
;--- 16h by a 55h that Windows made.  Those two want different stacks
;--- and there is no other way to tell them apart.  See RMMEM.INC.
rd_stkpsp   dw  RD_STKLVL dup (0)

            db  RD_STKSIZE dup(0)
rd_stk1:
            db  RD_STKSIZE dup(0)
rd_stk2:
            db  RD_STKSIZE dup(0)
rd_stk3:
            db  RD_STKSIZE dup(0)
rd_stktop:

_RD     ENDS
        END
