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
IFDEF EXIT_TRACE
        INCLUDE XTRACE.INC              ; BUILD exittrace's markers
ENDIF
IF LASTDRIVE NE RL_NDRV
        .ERR    <RMDOS.ASM: LASTDRIVE and RMDOS.INC's RL_NDRV disagree>
ENDIF

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
RD_STKLVL   EQU 3                       ; nesting levels rd_stkpsp remembers
RD_STKSLOTS EQU 6                       ; stacks there are - see stk_take
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
h_lowseg    dw  0
h_lowlen    dw  0
h_rsvd      dd  0
h_stubs     dd  0
h_wincache  dw  0                       ; RMH_WINCACHE - see RMCACHE.INC
h_wcflags   dw  0

        ORG     RMH_IRET
h_frame     db  RMI_LEN dup(0)

        ORG     RMH_DRIVES
h_drives    db  (LASTDRIVE * RMD_LEN) dup(0)

        ORG     RMH_LASTDRV
h_lastdrv   db  0                       ; LASTDRIVE=, 0 for no line
h_cdseg     dw  0                       ; RMCD.SYS, or 0 (RMINFO.INC)

        ORG     RMH_CTRYCODE            ; the country, as the kernel parked
h_ctrycode  dw  1                       ; with it: 38h, 65h and 66h answer
h_ctrypage  dw  437                     ; from here.  The United States until
h_ctrysys   dw  437                     ; the kernel says otherwise
h_ctryyes   db  'Y'
h_ctryno    db  'N'
rd_country:
        dw  0                           ; date format, month first
        db  '$',0,0,0,0                 ; currency
        db  ',',0                       ; thousands
        db  '.',0                       ; decimal
        db  '-',0                       ; date separator
        db  ':',0                       ; time separator
        db  0                           ; currency format
        db  2                           ; digits after the point
        db  0                           ; twelve-hour clock
        dd  0                           ; the case-map far call: rd_lowsave
                                        ; points it at the code the kernel
                                        ; left in the low block (RL_NLSMAP).
                                        ; Zero here was a call into the
                                        ; interrupt table, which WfW 3.11's
                                        ; network made
        db  ',',0                       ; list separator
        db  10 dup(0)

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
        jne     rd_notsig
        ; ...and the header in the layout this image was built for.
        ; A PMDOS.SYS and an RMDOS.SYS from different builds agree on
        ; the signature and on nothing after it, and a field read out
        ; of the wrong place is a worse failure than going home.
        cmp     word ptr [h_ver],RMDOS_VER
        je      rd_sigok
rd_notsig:

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

        ; The block below 64K, kept so that it can be put back; no
        ; block, or one of a size this image cannot keep, is a header
        ; this image was not built for.  See RMDOS.INC, RMH_LOWSEG.
        call    rd_lowsave
        jnc     rd_lowok
        mov     word ptr [h_why],RMW_NOTOURS
        jmp     rd_goback
rd_lowok:

        call    rd_vecs                 ; the vectors a DOS owns
        call    rd_hook13               ; ...and IO.SYS's hook on the disk
IFDEF EXIT_TRACE
        call    xt_hook                 ; and a real-mode CPU fault, caught
ENDIF
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
IFDEF RMDOS_E9
        call    e9_hook6                ; an invalid opcode says where
        call    e9_prehook              ; and what a driver's hook answers
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
        call    cm_init                 ; and the sector cache out of it,
                                        ; BEFORE Windows counts what is left
IFDEF RMDOS_E9
        call    e9_mcbs                 ; where we are, and what else is
ENDIF
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
        call    sda_sync                ; ...and the published copy agrees

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
        mov     ax,[h_runoff]           ; through a pointer, and not a jump
        mov     word ptr [rd_govec],ax  ; patched into the code - see
        mov     ax,[h_runseg]           ; rd_backvec below
        mov     word ptr [rd_govec + 2],ax

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
        jmp     dword ptr cs:[rd_govec]

rd_govec    dw  0, 0                    ; the program's entry, offset first

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
IFDEF EXIT_TRACE
        XTMARK16 52,047h                ; 'G'
ENDIF
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
IFDEF RMDOS_E9
        call    e9_unhook6              ; the instrument's own vector first
ENDIF
        call    rx_done                 ; the vector we took off the BIOS
        call    rd_unhook13             ; and the one on the disk
        call    rd_unchain              ; the links rd_chain made in drivers
                                        ; - PM-DOS's DOS data the kernel
                                        ; puts back itself (park_recover)
IFDEF EXIT_TRACE
        call    xt_unhook
        XTMARK16 53,04Ah                ; 'J'
ENDIF
        mov     ax,[rd_exitcode]
        mov     [h_exitcode],ax
        mov     ax,[h_unpoff]
        mov     word ptr [rd_backvec],ax
        mov     ax,[h_unpseg]
        mov     word ptr [rd_backvec + 2],ax
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
        jmp     dword ptr cs:[rd_backvec]

;---------------------------------------------------------------------
; THE WAY HOME WAS A JUMP PATCHED INTO THE CODE SIX BYTES AHEAD OF
; ITSELF, and a 386 or 486 does not see that.  It has already fetched
; and decoded the instructions in front of the one it is running, a
; store into them changes memory and not its copy, and Intel's answer
; is a jump between the store and the code it changed.  So the way back
; ran the jump as it was in the file - JMP FAR 0000:0000, into the
; vector table, with interrupts off: Charlie's 486 at a blinking cursor
; after Windows, the exittrace build's row stopped at "J".  QEMU, 86Box
; and DOSBox-X all see the store; even DOSBox-X's prefetch-queue CPUs
; refill at places that happened to miss it.  rd_go did the same 22
; bytes ahead.  Both jumps go through these instead, which are data.
;---------------------------------------------------------------------
rd_backvec  dw  0, 0                    ; RMH_UNPARK*, offset first

IFDEF EXIT_TRACE
;=====================================================================
; xt_hook / xt_unhook - BUILD exittrace (INC\XTRACE.INC): vectors 06h,
; 0Ch and 0Dh while RMDOS is the DOS, so that a CPU fault in REAL mode
; says where it was instead of becoming the loop it becomes on a real
; processor - 0Ch and 0Dh are IRQ 4's and IRQ 5's vectors there, whose
; handlers return to the instruction that faulted.
;
; Two tests keep it to faults.  The processor's PE bit (SMSW, which a
; V86 program may read): set, and this is Windows reflecting something
; into a virtual machine, which is Windows' business.  And for 0Ch and
; 0Dh the master PIC's in-service register: IRQ 4's or IRQ 5's bit set
; and it is that interrupt, which goes where it always went.
;=====================================================================
xt_hook:
        pushf
        cli
        push    ax
        push    es
        xor     ax,ax
        mov     es,ax
        mov     ax,es:[06h*4]
        mov     word ptr cs:[xt_old06],ax
        mov     ax,es:[06h*4+2]
        mov     word ptr cs:[xt_old06+2],ax
        mov     ax,es:[0Ch*4]
        mov     word ptr cs:[xt_old0c],ax
        mov     ax,es:[0Ch*4+2]
        mov     word ptr cs:[xt_old0c+2],ax
        mov     ax,es:[0Dh*4]
        mov     word ptr cs:[xt_old0d],ax
        mov     ax,es:[0Dh*4+2]
        mov     word ptr cs:[xt_old0d+2],ax
        mov     word ptr es:[06h*4],OFFSET xt_i06
        mov     es:[06h*4+2],cs
        mov     word ptr es:[0Ch*4],OFFSET xt_i0c
        mov     es:[0Ch*4+2],cs
        mov     word ptr es:[0Dh*4],OFFSET xt_i0d
        mov     es:[0Dh*4+2],cs
        pop     es
        pop     ax
        popf
        ret

xt_unhook:
        pushf
        cli
        push    ax
        push    es
        xor     ax,ax
        mov     es,ax
        mov     ax,word ptr cs:[xt_old06]
        mov     es:[06h*4],ax
        mov     ax,word ptr cs:[xt_old06+2]
        mov     es:[06h*4+2],ax
        mov     ax,word ptr cs:[xt_old0c]
        mov     es:[0Ch*4],ax
        mov     ax,word ptr cs:[xt_old0c+2]
        mov     es:[0Ch*4+2],ax
        mov     ax,word ptr cs:[xt_old0d]
        mov     es:[0Dh*4],ax
        mov     ax,word ptr cs:[xt_old0d+2]
        mov     es:[0Dh*4+2],ax
        pop     es
        pop     ax
        popf
        ret

xt_i06:
        push    bp
        mov     bp,06h
        jmp     short xt_real
xt_i0c:
        push    bp
        mov     bp,0Ch
        jmp     short xt_irq
xt_i0d:
        push    bp
        mov     bp,0Dh
xt_irq:
        push    ax
        mov     al,0Bh                  ; OCW3: the in-service register
        out     20h,al
        in      al,20h
        mov     ah,al
        mov     al,0Ah                  ; and the request register again,
        out     20h,al                  ; which is what a BIOS expects
        mov     al,10h                  ; IRQ 4
        cmp     bp,0Ch
        je      xt_irqbit
        mov     al,20h                  ; IRQ 5
xt_irqbit:
        test    ah,al
        pop     ax
        jnz     xt_chain
xt_real:
        push    ax
        smsw    ax
        test    al,1
        pop     ax
        jz      xt_fault
xt_chain:
        cmp     bp,0Ch
        je      xt_ch0c
        cmp     bp,0Dh
        je      xt_ch0d
        pop     bp
        jmp     dword ptr cs:[xt_old06]
xt_ch0c:
        pop     bp
        jmp     dword ptr cs:[xt_old0c]
xt_ch0d:
        pop     bp
        jmp     dword ptr cs:[xt_old0d]

        ; A fault, in real mode.  On the stack: the caller's BP, then the
        ; fault's IP, CS and FLAGS.  Row 1 gets "F", the vector, CS:IP,
        ; the four bytes there, AX BX CX DX SI DI BP DS ES SS SP; then the
        ; machine stops, which is what it was doing anyway.
xt_fault:
        cli
        mov     word ptr cs:[xt_vec],bp
        mov     word ptr cs:[xt_ax],ax
        mov     word ptr cs:[xt_bx],bx
        mov     word ptr cs:[xt_cx],cx
        mov     word ptr cs:[xt_dx],dx
        mov     word ptr cs:[xt_si],si
        mov     word ptr cs:[xt_di],di
        mov     word ptr cs:[xt_ds],ds
        mov     word ptr cs:[xt_es],es
        mov     word ptr cs:[xt_ss],ss
        pop     ax
        mov     word ptr cs:[xt_bp],ax
        pop     ax
        mov     word ptr cs:[xt_ip],ax
        pop     ax
        mov     word ptr cs:[xt_cs],ax
        mov     ax,sp
        add     ax,2                    ; as it was before the fault's frame
        mov     word ptr cs:[xt_sp],ax
        mov     ax,0B800h
        mov     es,ax
        mov     di,160
        cld
        mov     ax,4F00h + 'F'          ; and the vector straight after it:
        stosw                           ; the line is the row's 80 exactly
        mov     ax,word ptr cs:[xt_vec]
        call    xt_hexw
        mov     ax,word ptr cs:[xt_cs]
        call    xt_hexw
        mov     ax,word ptr cs:[xt_ip]
        call    xt_hexw
        mov     ds,word ptr cs:[xt_cs]
        mov     si,word ptr cs:[xt_ip]
        mov     cx,4
xtf_code:
        lodsb
        mov     ah,al
        shr     al,4
        call    xt_nib
        mov     al,ah
        call    xt_nib
        loop    xtf_code
        mov     ax,4F20h
        stosw
        mov     si,OFFSET xt_ax
        mov     cx,(xt_end - xt_ax) / 2
xtf_regs:
        mov     ax,word ptr cs:[si]
        call    xt_hexw
        add     si,2
        loop    xtf_regs
xtf_stop:
        cli
        hlt
        jmp     short xtf_stop

;--- AX in hex at ES:DI, white on red, and a blank after it
xt_hexw:
        push    cx
        mov     cx,4
xth_one:
        rol     ax,4
        push    ax
        call    xt_nib
        pop     ax
        loop    xth_one
        push    ax
        mov     ax,4F20h
        stosw
        pop     ax
        pop     cx
        ret

;--- AL's low nibble in hex at ES:DI
xt_nib:
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     xtn_dig
        add     al,7
xtn_dig:
        mov     ah,4Fh
        stosw
        pop     ax
        ret

xt_old06    dd  0
xt_old0c    dd  0
xt_old0d    dd  0
xt_vec      dw  0
xt_cs       dw  0
xt_ip       dw  0
xt_ax       dw  0                       ; and from here in the order shown
xt_bx       dw  0
xt_cx       dw  0
xt_dx       dw  0
xt_si       dw  0
xt_di       dw  0
xt_bp       dw  0
xt_ds       dw  0
xt_es       dw  0
xt_ss       dw  0
xt_sp       dw  0
xt_end:
ENDIF

;=====================================================================
; rd_lowsave - adopt the block below 64K that PM-DOS handed over, and
;              keep the part of it this DOS reads.  CF set if there is
;              no usable block.
;
; The block is the kernel's DOS data segment and everything in it is
; the kernel's - its list of lists, its SDA, the file table and country
; tables its boxes read.  RMDOS overwrites the first RL_LEN bytes of it
; with its own versions of the same things for the length of the
; excursion (RMINFO.INC, rd_sysinit), and THE KERNEL PUTS THEM BACK:
; it keeps the RMH_LOWLEN bytes itself, above the megabyte, from the
; handover to its recovery (PARK.INC, park_lowkeep).  This side used to
; keep them in its own 64K and restore them on the way home - until the
; twenty-six letters Windows' network maps to made the copy 4K and the
; traced build no longer fitted the segment (2026-10-09).
;
; What is kept here is only what rd_sysinit and rd_chain read before
; they write over it: the kernel's current directories for the drives
; this DOS has volumes for, and NUL's next link - RD_KEEPLEN bytes from
; the start, at the same offsets.  RMH_LOWLEN is still checked: no less
; than RMDOS builds in, and no more than the kernel says it keeps.
;=====================================================================
rd_lowsave:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    es
        mov     ax,[h_lowseg]
        or      ax,ax
        jz      rls_bad
        mov     cx,[h_lowlen]
        cmp     cx,RL_LEN
        jb      rls_bad
        cmp     cx,RL_SAVEMAX
        ja      rls_bad
        mov     [rd_lowseg],ax
        mov     [rd_lowlen],cx
        ; The country block's case-map far call (38h and 6501h hand it
        ; out) points at the twelve bytes the kernel left in the block
        ; for it, where the kernel points its own (GNLS_CASEMAP): a
        ; character from 80h up comes back as the country's table has
        ; it.  It was zero until WfW's network called it - a far call
        ; to 0000:0000 runs the interrupt table as code - and a bare
        ; far return while the table was the identity.
        mov     word ptr [rd_country+12h],RL_NLSMAP
        mov     word ptr [rd_country+14h],ax
        mov     di,cs
        mov     es,di
        mov     di,OFFSET rd_lowkeep
        mov     ds,ax
        xor     si,si
        mov     cx,RD_KEEPLEN
        cld
        rep     movsb
        clc
        jmp     short rls_out
rls_bad:
        stc
rls_out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

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
; UNDER WHATEVER A DOS PROGRAM PUT IN FRONT OF PM-DOS, NOT IN ITS
; PLACE.  A DOS box's INT 21h goes through its own vector table, and
; each of these vectors points at a stub of PM-DOS's (h_stubs) until
; somebody hooks it.  A vector still pointing at its stub is simply
; taken.  One pointing anywhere else has been hooked - a TSR, SETVER,
; a DOSKEY loaded in the box - and the hook chains to the stub when it
; is done, so the STUB becomes a JMP FAR to the handler here and the
; chain runs into this DOS as it ran into that one.  This used to set
; all sixteen outright, which unhooked every resident program for as
; long as Windows ran.  PM-DOS puts the table and the stubs back on
; the way home.
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
        shl     di,2                    ; ES:DI = the vector's slot
        mov     cl,al                   ; CL = the vector
        lodsw
        mov     dx,ax                   ; DX = the handler here
        mov     bx,word ptr [h_stubs+2]
        or      bx,bx
        jz      rv_take                 ; no stubs: nothing to go under
        mov     al,cl
        sub     al,20h
        mov     ah,RMS_STUBLEN
        mul     ah
        add     ax,word ptr [h_stubs]   ; BX:AX = this vector's stub

        ; EVERY STUB JUMPS HERE, whether or not the vector points at it
        ; now.  Until v0.60 only a hooked vector's stub was rewritten,
        ; and an unhooked one's kept its HLT - harmless until Windows
        ; started a DOS program.  To do that it puts back the interrupt
        ; table it saved when WIN.COM started, and WIN.COM started on
        ; PM-DOS, where INT 21h IS the stub: so the first DOS call of
        ; every DOS session was a HLT with no monitor behind it, and the
        ; CPU ran on from there into bytes that were never code.
        push    es
        push    di
        mov     es,bx
        mov     di,ax
        cli
        mov     byte ptr es:[di],0EAh   ; JMP FAR cs:handler
        mov     word ptr es:[di+1],dx
        mov     word ptr es:[di+3],cs
        sti
        pop     di
        pop     es
        cmp     word ptr es:[di],ax
        jne     rv_loop                 ; hooked: the hook reaches the stub
        cmp     word ptr es:[di+2],bx
        jne     rv_loop
rv_take:
        cli
        mov     word ptr es:[di],dx
        mov     word ptr es:[di+2],cs
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

;--- 13h - the disk BIOS, reached through rd_orig13.  IO.SYS keeps a
;--- hook of its own on INT 13h from the moment it boots, and that hook
;--- goes on to "Orig13", which INT 2Fh AH=13h hands out and replaces
;--- (RMINFO.INC, i2F_disk13): the way a driver - Windows' BLOCKDEV.386
;--- among them - puts itself in front of every disk call DOS makes.
;--- This is that hook, and this DOS's own INT 13h calls all come
;--- through it.  rd_hook13 puts it in the table, rd_unhook13 takes it
;--- out on the way home.
i13:
        jmp     dword ptr cs:[rd_orig13]

rd_hook13:
        push    eax
        push    es
        xor     ax,ax
        mov     es,ax
        cli
        mov     eax,dword ptr es:[13h*4]
        mov     dword ptr [rd_ivt13],eax        ; to put back
        mov     dword ptr [rd_orig13],eax       ; Orig13 and Old13, the
        mov     dword ptr [rd_old13],eax        ; BIOS's own both
        mov     word ptr es:[13h*4],OFFSET i13
        mov     word ptr es:[13h*4+2],cs
        sti
        pop     es
        pop     eax
        ret

; Only if the vector is still this one: anything that hooked INT 13h in
; front of it would have its own way back, and is not this DOS's to
; undo.  rd_goback runs with interrupts off.
rd_unhook13:
        push    eax
        push    es
        xor     ax,ax
        mov     es,ax
        cmp     word ptr es:[13h*4],OFFSET i13
        jne     ruh_out
        mov     ax,cs
        cmp     word ptr es:[13h*4+2],ax
        jne     ruh_out
        mov     eax,dword ptr [rd_ivt13]
        mov     dword ptr es:[13h*4],eax
ruh_out:
        pop     es
        pop     eax
        ret

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

        ; THE CRITICAL SECTION BEFORE ANYTHING ELSE.  Everything below -
        ; the process state pulled out of the SDA, InDOS, the depth, the
        ; stack slots - is one copy for every virtual machine, and it is
        ; the critical section (crit_in) that makes it one machine's at
        ; a time.  It used to be taken further down, after all of that,
        ; so the next machine to call pulled its own state over the
        ; working copies of one still inside a call and holding the
        ; section, then waited for the section with the damage done.
        ; The port E9h trace caught it: a DOS box ending (4Ch, inside
        ; IFSHLP's 2Fh/1122h) when Windows' System VM called; the box's
        ; terminate wrote the System VM's state into the box's SDA and
        ; let go, and the System VM's call went on with the box's
        ; parent as its process - its handles invalid, InDOS and the
        ; depth zero inside a call.  That was the DOS-box stall: a box's
        ; redirected output gone, "EXEC failure", 0-byte copies, and
        ; Windows unable to read a segment of USER.EXE through a handle
        ; that was no longer its own ("Segment Load Failure",
        ; 2026-10-08).
IFDEF RMDOS_E9
        call    e9_i21pre               ; arrived - before the section
ENDIF                                   ; can make it wait (RME9.INC)
        call    crit_in
        cli

        ; THE PROCESS STATE IS READ OUT OF THE PUBLISHED SDA FIRST, not
        ; out of this file's variables, because under Windows the SDA
        ; is INSTANCED: DOSMGR gives every virtual machine its own copy
        ; of the bytes 5D06h names and swaps them in and out as it
        ; switches, so the current PSP, DTA and drive that apply to
        ; this call are whatever it swapped in last.  sda_pull brings
        ; them into the variables the rest of this DOS reads, and
        ; i21_out puts them back with sda_sync.  See RMINFO.INC.
        call    sda_pull
        inc     byte ptr [rd_indos]
        call    sda_sync

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

        ; A STACK SLOT, NOT A STACK LEVEL.  This used to take stack
        ; number [rd_stkdep], which is right as long as calls nest and
        ; unwind in order - and under Windows they do not.  Two virtual
        ; machines can each be waiting at a prompt, which is each
        ; standing inside INT 21h 0Ah on a stack of ours; the one that
        ; gets a key first finishes first, and the next call in would
        ; have been given the stack the other is still on.  So stk_take
        ; hands out the first free of RD_STKSLOTS and stk_free gives it
        ; back by looking at where SP is; the depth counter stays, for
        ; rd_stkpsp and nothing else.  No slot free: stay on the
        ; caller's stack, which is what DOS did too.
        call    stk_take                ; -> AX = the top of one, CF if none
        jc      i21_stacked
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
IFDEF RMDOS_E9
        call    e9_i21in                ; ...and a line on port E9h, RME9.INC
ENDIF

        mov     ax,[bp+F_AX]
        call    sda_userax              ; for a redirector - RMNET.INC
        mov     bl,byte ptr [bp+F_AX+1]
        movzx   bx,bl
        cmp     bx,I21_MAX
        jae     i21_bad
        shl     bx,1
        call    word ptr [i21_tab + bx]
        jmp     short i21_out
i21_bad:
        ; A FUNCTION NUMBER PAST THE TABLE IS ANSWERED AS MS-DOS ANSWERS
        ; IT: AL=0 and the carry clear (DOS6SRC dos/msdisp.asm, BadCall,
        ; which says it cannot set the carry "as this may break some
        ; programs compatability").  WIN386 asks AH=DCh - NetWare's "get
        ; connection number" - as it starts, and read the ERR_FUNC this
        ; used to answer as a connection number (2026-10-06).  The
        ; functions INSIDE the table that are not served still fail
        ; with ERR_FUNC and the carry, as DOS's own do.
        mov     byte ptr [bp+F_AX],0
        call    i21_ok

i21_out:
        cli
IFDEF RMDOS_E9
        call    e9_i21out               ; the answer, on the same line
ENDIF
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
        call    sda_sync                ; the published SDA, for the next
                                        ; caller and for DOSMGR's swap
        call    crit_out                ; and another machine may have DOS
        call    stk_free                ; this stack is nobody's again
        mov     ss,ax
        mov     sp,bx
        pop     es
        pop     ds
        popa
        iret

;=====================================================================
; crit_in / crit_out - a DOSMGR critical section round every DOS call.
;
; Windows in 386 enhanced mode switches between its machines whenever
; it likes, and what stops it switching away from one that is in the
; middle of DOS is DOS saying so: INT 2Ah AH=80h begins a critical
; section and AH=81h ends one, DOSMGR hooks both, and MS-DOS 5 calls
; them round the parts of itself that cannot be re-entered.  RMDOS
; told DOSMGR it was a version 5 DOS (RMINFO.INC, the 1607h callouts),
; so DOSMGR expects those calls and does no patching of its own; and
; nearly all of RMDOS cannot be re-entered - the directory buffer, the
; FAT window, the path being resolved are one of each.  So the whole
; of every call is a critical section, and the one place a call lets
; go of the machine on purpose - the console wait's INT 28h - lets go
; of the section first and takes it back after (RMCON.INC).  Outside
; Windows INT 2Ah is RMDOS's own IRET and these cost two instructions.
;=====================================================================
crit_in:
        push    ax
        mov     ax,8001h
        int     2Ah
        pop     ax
        ret

crit_out:
        push    ax
        mov     ax,8101h
        int     2Ah
        pop     ax
        ret

;=====================================================================
; stk_take - the first free stack slot: AX = its top, CF if there is
;            none.  stk_free gives back the slot SS:SP is standing in,
;            or does nothing if that is not one of ours.
;
; Both preserve every register but AX (stk_take) and the flags.
;=====================================================================
stk_take:
        push    bx
        push    cx
        mov     bl,[rd_stkmap]
        xor     cx,cx
stt_scan:
        shr     bl,1
        jnc     stt_got
        inc     cx
        cmp     cx,RD_STKSLOTS
        jb      stt_scan
        pop     cx
        pop     bx
        stc
        ret
stt_got:
        mov     ax,1
        shl     ax,cl
        or      [rd_stkmap],al
        inc     cx
        mov     ax,cx                   ; the top of slot CX-1 - a SHIFT,
        shl     ax,10                   ; not a MUL: the caller's frame
        add     ax,OFFSET rd_stks       ; offset is in DX at this point
        pop     cx
        pop     bx
        clc
        ret

stk_free:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     ax,sp
        sub     ax,OFFSET rd_stks
        jb      stf_out                 ; below the slots: not ours
        cmp     ax,RD_STKSLOTS * RD_STKSIZE
        jae     stf_out                 ; above them: not ours either
        xor     dx,dx
        mov     bx,RD_STKSIZE
        div     bx                      ; AX = the slot SP is in
        mov     cx,ax
        mov     al,1
        shl     al,cl
        not     al
        and     [rd_stkmap],al
stf_out:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

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
        dw  i21_unsup, fn19,      fn1A,      fn1B        ; 18-1B
        dw  fn1C,      i21_unsup, i21_unsup, fn1F        ; 1C-1F
        dw  i21_unsup, i21_unsup, i21_unsup, i21_unsup   ; 20-23
        dw  i21_unsup, fn25,      fn26,      i21_unsup   ; 24-27
        dw  i21_unsup, fn29,      fn2A,      fn2B        ; 28-2B
        dw  fn2C,      fn2D,      fn2E,      fn2F        ; 2C-2F
        dw  fn30,      fn31,      fn32,      fn33        ; 30-33
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
        dw  fn5C,      fn5D,      fn5E,      fn5F        ; 5C-5F
        dw  fn60,      i21_unsup, fn51,      fn63        ; 60-63
        dw  i21_unsup, fn65,      fn66,      fn67        ; 64-67
        dw  fn68,      fn69,      fn68,      i21_unsup   ; 68-6B
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
        INCLUDE RMDEV.INC               ; the drivers CONFIG.SYS loaded -
                                        ; before RMFILE2, whose handle calls
                                        ; use its equates, which wasm will
                                        ; not take before they are defined
        INCLUDE RMFILE2.INC             ; ...and the handles and calls
        INCLUDE RMNAME.INC              ; MD, RD, rename, TRUENAME, 29h
        INCLUDE RMMEM.INC               ; the arena, EXEC and terminate
        INCLUDE RMINFO.INC              ; everything that only answers
        INCLUDE RMNET.INC               ; a redirector's drives and files -
                                        ; after RMINFO, whose SDA equates
                                        ; it uses
        INCLUDE RMXMS.INC               ; extended memory, A20 and INT 15h
        INCLUDE RMCACHE.INC             ; after it: the sector cache lives in
                                        ; that memory and moves through it
IFDEF RMDOS_E9
        INCLUDE RME9.INC                ; the port E9h trace - build "rme9"
ENDIF
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
rd_ncds     db  LASTDRIVE               ; letters there are, LASTDRIVE= as
                                        ; MS-DOS counts it (rd_sysinit)
rd_lasterr  dw  0
rd_verify   db  0
rd_brkchk   db  0
rd_exitcode dw  0
rd_exitway  db  0
rd_dtaseg   dw  0
rd_dtaoff   dw  80h
rd_orig13   dd  0                       ; what i13 goes on to: Orig13
rd_old13    dd  0                       ; ...the vector a reboot would want
rd_ivt13    dd  0                       ; ...and the table's, when we came
rd_lowseg   dw  0                       ; the block below 64K - RMH_LOWSEG,
rd_lowlen   dw  0                       ; once rd_lowsave has accepted it
rd_lowkeep  db  RD_KEEPLEN dup(0)       ; the kernel's CDS for our volumes,
                                        ; and NUL's link (rd_lowsave)
IF RL_NULDEV + 4 GT RD_KEEPLEN
        .ERR    <RMDOS.ASM: NUL's link is past what rd_lowsave keeps>
ENDIF

msg_hello   db  13,10,'RMDOS is the DOS in this machine now.',13,10,0

rd_stkmap   db  0                       ; a bit per slot, set while a call
                                        ; is standing on it

;--- WHO IS WAITING IN WHICH CALL.  One PSP per nesting level, written
;--- at i21 entry and read by term_common: a process that ends has to
;--- know whether the parent it names is standing inside a DOS call -
;--- an EXEC that has not returned - or is simply the PSP written at
;--- 16h by a 55h that Windows made.  Those two want different stacks
;--- and there is no other way to tell them apart.  See RMMEM.INC.
rd_stkpsp   dw  RD_STKLVL dup (0)

rd_stks     db  RD_STKSLOTS * RD_STKSIZE dup(0)     ; slot 0 ends at
rd_stktop:                                          ; rd_stks + RD_STKSIZE

_RD     ENDS
        END
