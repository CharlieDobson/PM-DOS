;=====================================================================
; IOPM.ASM - PMIO.SYS protected-mode body
;
; The second half of PMIO.SYS.  Linked as a raw binary based at
; IO_PM_BASE and concatenated onto IORM.BIN.  Because IORM.BIN is
; padded to exactly RMSTUB_SIZE bytes, everything here lands where the
; linker was told it would.
;
; The image starts with a JMP to the entry point and a table of service
; routine addresses at IO_PM_BASE+10h.  PMDOS.SYS calls into PMIO
; only through that table, so the two files never need to be linked
; against each other.
;
; Linked with:  wlink form raw bin option offset=0x22000
;=====================================================================
        .386P

        INCLUDE PMDOS.INC

_PM     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_PM, DS:_PM, ES:_PM, SS:_PM
        ORG     0

;=====================================================================
; Image header
;=====================================================================
        jmp     pm_entry                ; IO_PM_BASE + 00h

        ORG     10h
;---------------------------------------------------------------------
; Service table - must match the IOS_* equates in PMDOS.INC
;---------------------------------------------------------------------
svc_table:
        dd      con_out                 ; IOS_CONOUT
        dd      con_str                 ; IOS_CONSTR
        dd      kb_getchar              ; IOS_CONIN
        dd      kb_status               ; IOS_CONSTAT
        dd      con_cls                 ; IOS_CLS
        dd      con_setcur              ; IOS_SETCUR
        dd      con_getcur              ; IOS_GETCUR
        dd      blk_read                ; IOS_RDBLK
        dd      blk_write               ; IOS_WRBLK
        dd      rm_int                  ; IOS_REALINT
        dd      get_ticks               ; IOS_TICKS
        dd      blk_params              ; IOS_DRVPARM
        dd      set_vector              ; IOS_SETVEC
        dd      panic                   ; IOS_PANIC
        dd      v86_run                 ; IOS_V86RUN
        dd      v86_intctx              ; IOS_V86INT
        dd      pg_a20                  ; IOS_A20
        dd      pg_a20get               ; IOS_A20GET
        dd      blk_geo                 ; IOS_DRVGEO
        dd      con_getsize             ; IOS_SCRSIZE
        dd      pm3_sethook             ; IOS_PM3HOOK
        dd      exc_sethook             ; IOS_EXCHOOK
        dd      exc_clearnest           ; IOS_EXCCLEAR
        dd      nt_arm                  ; IOS_NULLTRAP
        dd      nt_get                  ; IOS_NULLGET
        dd      kb_boxreset             ; IOS_KBBOX
        dd      pg_map                  ; IOS_PGMAP
        dd      pg_umbprobe             ; IOS_UMBPROBE
        dd      kb_typematic            ; IOS_KBRATE
        dd      ata_config              ; IOS_ATACFG
        dd      ata_info                ; IOS_ATAINFO

;=====================================================================
; pm_entry - first instruction executed in 32-bit protected mode.
;
; Reached by a far JMP from the real-mode stub.  CS is SEL_CODE32,
; every other segment register is still holding real-mode garbage and
; interrupts are off.
;=====================================================================
pm_entry:
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        mov     ss,ax
        mov     esp,KSTACK_TOP
        cld

        call    con_init                ; clears the screen, so say it
IFDEF PMDOS_DEBUG                       ; again on the fresh one
        mov     esi,OFFSET msgPM
        call    con_str
ELSE
        mov     esi,OFFSET msgStart2
        call    con_str
ENDIF

        call    idt_init
        lidt    fword ptr [pm_idtr]
        ; Paging before the TSS, and well before any DOS box: the
        ; identity map changes nothing anyone can see, but the sixteen
        ; entries it puts under the A20 gate are the only thing that
        ; stops a guest reading the 1MB wrap and finding kernel memory.
        call    pg_init
        call    nt_init                 ; and the null guard, which is four
                                        ; debug registers rather than the page
                                        ; the roadmap wanted - NULLTRAP.INC
                                        ; says why page zero cannot go
        call    v86_init                ; the TSS, before anything can fault

        call    pic_init
        call    kb_init
        call    pci_init                ; before blk_init, which probes the
                                        ; disks and asks each one how it
                                        ; would rather be talked to
        call    blk_init
IFDEF PMDOS_DEBUG
        mov     esi,OFFSET msgATA
        call    con_str
        mov     eax,[ata_count]
        add     al,'0'
        call    con_out
        mov     esi,OFFSET msgATA2
        call    con_str
ENDIF

        sti

;--- sanity check PMDOS.SYS before handing over ----------------------
        mov     eax,dword ptr ds:[GV_DOSSIZE]
        or      eax,eax
        jnz     dos_ok
        mov     esi,OFFSET msgNoDos
        jmp     panic
dos_ok:
IFDEF PMDOS_DEBUG
        mov     esi,OFFSET msgStart
        call    con_str
ENDIF
        mov     eax,DOS_BASE
        jmp     eax

;=====================================================================
; set_vector - install a software interrupt handler.
;              AL = vector, EDI = handler.  Gate is DPL 3 so that
;              unprivileged code can INT it once we grow a ring 3.
;=====================================================================
set_vector:
        push    edx
        mov     edx,edi
        mov     ah,0EEh
        call    set_gate
        pop     edx
        ret

;=====================================================================
; get_ticks - EAX = the 18.2 Hz tick counter maintained by IRQ 0.
;
; Straight out of the BIOS data area, which as of v0.32 is where that
; counter lives.  The kernel used to keep a private one that started
; at zero on boot while 40:6C stood still; there is now one clock, and
; this reads it.
;=====================================================================
get_ticks:
        mov     eax,dword ptr ds:[BDA_TICKS]
        ret

;=====================================================================
; pic_init - remap the 8259s clear of the CPU exception vectors and
;            unmask only the timer and the keyboard.
;=====================================================================
pic_init:
        mov     al,11h                  ; ICW1: cascade, edge, ICW4 follows
        out     PIC1_CMD,al
        call    io_delay
        out     PIC2_CMD,al
        call    io_delay

        mov     al,IRQ_BASE             ; ICW2: vector bases
        out     PIC1_DATA,al
        call    io_delay
        mov     al,IRQ_BASE2
        out     PIC2_DATA,al
        call    io_delay

        mov     al,4                    ; ICW3: slave is wired to IR2
        out     PIC1_DATA,al
        call    io_delay
        mov     al,2
        out     PIC2_DATA,al
        call    io_delay

        mov     al,1                    ; ICW4: 8086 mode
        out     PIC1_DATA,al
        call    io_delay
        out     PIC2_DATA,al
        call    io_delay

        mov     al,0FCh                 ; leave IRQ0 and IRQ1 enabled
        out     PIC1_DATA,al
        call    io_delay
        mov     al,0FFh
        out     PIC2_DATA,al
        ret

;---------------------------------------------------------------------
; io_delay - a short pause for 8259s and other slow ISA parts.
;---------------------------------------------------------------------
io_delay:
        push    eax
        in      al,80h
        in      al,80h
        pop     eax
        ret

;=====================================================================
; panic - print ESI and stop the machine for good.
;=====================================================================
panic:
        cli
        push    esi
        mov     esi,OFFSET msgPanic
        call    con_str
        pop     esi
        call    con_str
        mov     al,13
        call    con_out
        mov     al,10
        call    con_out
panic_hlt:
        hlt
        jmp     panic_hlt

;=====================================================================
        INCLUDE EXCEPT.INC
        INCLUDE PAGING.INC
        INCLUDE NULLTRAP.INC
        INCLUDE VM86.INC
        INCLUDE CONSOLE.INC
        INCLUDE KEYBD.INC
        INCLUDE RMCALL.INC
        INCLUDE PCI.INC
        INCLUDE ATA.INC
        INCLUDE BLOCK.INC

;=====================================================================
; Data
;=====================================================================
pm_idtr:
        dw      IDT_LIMIT
        dd      IDT_BASE


IFDEF PMDOS_DEBUG
msgPM       db  'PMIO: 32-bit protected mode entered',13,10,0
msgStart    db  'PMIO: starting PMDOS.SYS',13,10,0
msgATA      db  'PMIO: IDE disks found: ',0
msgATA2     db  ' (0 = BIOS INT 13h fallback)',13,10,0
ELSE
msgStart2   db  'Starting PM-DOS...',13,10,0
ENDIF
msgNoDos    db  'PMDOS.SYS was never loaded',0
msgPanic    db  13,10,'PMIO PANIC: ',0

_PM     ENDS
        END
