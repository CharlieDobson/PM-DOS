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
        ORG     0

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
        ORG     0100h

;=====================================================================
; rd_entry - the first instruction executed in real mode after the
;            handover.  CS = our segment and nothing else is promised.
;=====================================================================
rd_entry:
        cli
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
        mov     word ptr [h_why],RMW_EXIT
IFDEF RMDOS_BISECT
        jmp     rd_goback               ; straight back out: does the
                                        ; SWITCH work, before asking
                                        ; whether the DOS does
ENDIF

        call    rd_vecs                 ; the vectors a DOS owns
        call    rd_volinit              ; per-drive geometry
        call    rd_sysinit              ; SDA, list of lists, CDS
        call    rd_sftinit              ; the handle table and its devices
        call    rx_init                 ; extended memory, and the A20 gate

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
rd_back:
        db      0EAh                    ; JMP FAR <patched>:<patched>
        dw      0
        dw      0

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
i21:
        cli
        pusha
        push    ds
        push    es
        mov     bp,sp
        mov     cx,ss                   ; where the caller's frame is
        mov     dx,bp

        mov     ax,cs
        mov     ds,ax
        inc     byte ptr [rd_indos]

        mov     al,[rd_stkdep]
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
        dw  fn54,      i21_unsup, fn56,      fn57        ; 54-57
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
