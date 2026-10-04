;=====================================================================
; AHA154X.INC - the Adaptec AHA-154x, and BusLogic, by their mailboxes
;
; The 1542 is the card DOS-era SCSI was written for, and BusLogic made
; theirs speak its interface so that the same drivers would work.  Four
; I/O ports and one idea: the card reads its work out of memory.  The
; host writes a Command Control Block (CCB) - target, LUN, the SCSI
; command block, where the data is - puts the CCB's address in an
; OUT-MAILBOX, and tells the card to look (command 02h).  The card
; masters the bus, moves the data, writes the target's status and any
; sense data back into the CCB, and puts the CCB's address in an
; IN-MAILBOX with a word on how it went.  One mailbox each way and one
; CCB per card is enough for a kernel that has one request in flight.
;
; POLLED, like everything else here: the card's interrupt is never
; unmasked, and the in-mailbox is watched instead.  Its status byte is
; written by the card's DMA, and the processor sees that write - an
; x86 keeps bus-master writes coherent with its caches.
;
; TWENTY-FOUR BITS OF ADDRESS.  An ISA card can reach the first 16MB
; and no more, so the mailboxes and CCBs are in PMIO (at 2MB) and the
; data goes through SCSI_BUF (SHF_BOUNCE: SCSI.INC does the copying).
; A BusLogic card has a 32-bit mode of its own, which would avoid the
; copy; the 24-bit mode works on every one of them, and a CD-ROM is
; not fast enough for the copy to show.
;
; WHO ELSE MIGHT BE USING THE CARD.  Three parties, and none of them is
; disturbed:
;
;   THE CARD'S ROM, serving a SCSI hard disk through INT 13h.  The 1542C
;   and later run their BIOS through a mailbox of their own (commands
;   25h and 82h), the 1542A/B through command 03h, BusLogic through 83h
;   - none of them through the mailboxes set up here, so a SCSI C: and
;   a CD-ROM on the same card both work.
;
;   AN ASPI MANAGER loaded by DEVICE=.  It sets up mailboxes of its own
;   when it loads, and resetting the card would take them away from it.
;   So when one is loaded (SPF_ASPI) the probe asks the card first -
;   status INIT clear and a mailbox count in Return Setup Data mean
;   somebody owns it - and leaves an owned card to the manager.
;
;   ANYBODY WHO RESETS IT behind PMIO's back - Windows, while the kernel
;   is parked, or a program in a box.  The card then asks for mailboxes
;   again (status INIT), and aha_exec gives it them before each request
;   that finds it asking.
;
; Linux's aha1542.c was the reference for what the hardware does, and
; 86Box's x54x emulation for what the tests ran on.
;
; Included by IOPM.ASM, after SCSI.INC.
;=====================================================================

; the ports, from the base
AHR_STAT        EQU     0               ; read: status; write: control
AHR_DATA        EQU     1               ; write: commands; read: replies
AHR_INTR        EQU     2               ; read: interrupt flags

; status
AHS_STST        EQU     80h             ; self-test running
AHS_DIAGF       EQU     40h             ; ...and it failed
AHS_INIT        EQU     20h             ; mailboxes wanted
AHS_IDLE        EQU     10h
AHS_CDF         EQU     08h             ; command port still full
AHS_DF          EQU     04h             ; a reply byte is waiting
AHS_INVDCMD     EQU     01h             ; that command was not one
AHS_MASK        EQU     0FDh            ; bit 1 is reserved

; interrupt flags
AHI_HACC        EQU     04h             ; a command has finished
AHI_MASK        EQU     8Fh

; control
AHC_HRST        EQU     80h
AHC_SRST        EQU     40h
AHC_IRST        EQU     20h             ; clear the interrupt flags
AHC_SCRST       EQU     10h             ; reset the SCSI bus

; commands
AHK_MBINIT      EQU     01h             ; count, address (3)
AHK_START       EQU     02h             ; look at the out-mailboxes
AHK_INQUIRY     EQU     04h             ; -> four bytes, the board's id
AHK_RETCONF     EQU     0Bh             ; -> DMA, IRQ, own SCSI ID
AHK_RETSETUP    EQU     0Dh             ; length -> that much setup data
AHK_EXTBIOS     EQU     28h             ; 1542C: -> flags, lock code
AHK_MBENABLE    EQU     29h             ; ...and the unlock
AHK_BLVER3      EQU     84h             ; BusLogic only: a firmware digit
AHK_BLPCI       EQU     86h             ; BusLogic PCI: its ISA-style port

; one card's block in memory: a mailbox each way, then the CCB
AHB_MBO         EQU     0               ; status, CCB address (3, big-endian)
AHB_MBI         EQU     4
AHB_CCB         EQU     8
AHB_LEN         EQU     64

; the CCB, 24-bit form
CCB_OP          EQU     0               ; 00h: an initiator command
CCB_IDLUN       EQU     1               ; target in 7-5, LUN in 2-0
CCB_CDBLEN      EQU     2
CCB_RSALEN      EQU     3               ; sense bytes to fetch on CHECK
CCB_DLEN        EQU     4               ; 3 bytes, big-endian
CCB_DPTR        EQU     7               ; 3 bytes
CCB_HASTAT      EQU     14              ; out: the card's own status
CCB_TSTAT       EQU     15              ; out: the target's status byte
CCB_CDB         EQU     18              ; the command block, then the sense
CCB_SENSELEN    EQU     18

; the card's own word on a request (CCB_HASTAT)
AHH_SELTO       EQU     11h             ; nothing answered selection
AHH_UNDER       EQU     12h             ; data over- or underrun

; IN-MAILBOX status
AHM_ABORTED     EQU     2
AHM_NOTFOUND    EQU     3

; how long to wait, counted in reads of the card's own ports - an ISA
; read is the closest thing to a clock a polled driver has, as it is
; for ATA.INC
AHW_BYTE        EQU     00200000h       ; a command or reply byte
AHW_RESET       EQU     00400000h       ; a reset to finish
AHW_IO          EQU     01000000h       ; a SCSI request: a drive spinning
                                        ; up takes seconds
AHW_SETTLE      EQU     20000           ; reads of port 80h after a reset

;=====================================================================
; aha_probe - every Adaptec or BusLogic card PMIO can find, registered.
;             All registers kept.
;
; THE PCI BUS FIRST.  A BusLogic PCI card may ALSO answer at one of the
; ISA addresses ("ISA compatible port"), and found there as well it
; would be two adapters with the same disc on each.  So each PCI card
; is asked where that port is (86h), and the ISA probe skips it.
;=====================================================================
aha_probe:
        pushad
        mov     dword ptr [aha_skip],0
        mov     dword ptr [aha_skip+4],0

        xor     ebp,ebp                 ; EBP = which PCI card next
apr_pci:
        mov     esi,ebp
        mov     edi,PCT_ID
        mov     edx,0FFFFFFFFh
        mov     ecx,1040104Bh           ; BusLogic MultiMaster
        call    pcx_match               ; -> EBX = its address
        jc      apr_isa
        call    aha_pcione
        inc     ebp
        cmp     ebp,SCSI_MAXHBA
        jb      apr_pci

apr_isa:
        mov     esi,OFFSET aha_ports
apr_port:
        movzx   edx,word ptr [esi]
        or      edx,edx
        jz      apr_done
        call    aha_taken               ; a PCI card's, or found already?
        jnc     apr_next
        call    aha_one                 ; DX = the port
apr_next:
        add     esi,2
        jmp     apr_port
apr_done:
        popad
        ret

;---------------------------------------------------------------------
; aha_taken - DX = an ISA port.  CF CLEAR if it is spoken for: in
;             aha_skip, or the base of an adapter already registered.
;             All registers kept.
;---------------------------------------------------------------------
aha_taken:
        push    eax
        push    ecx
        push    esi
        mov     esi,OFFSET aha_skip
        mov     ecx,4
atk_skip:
        cmp     dx,[esi]
        je      atk_yes
        add     esi,2
        loop    atk_skip
        mov     esi,OFFSET scs_hbas
        mov     ecx,[scs_nhba]
atk_hba:
        jecxz   atk_no
        cmp     dx,[esi+HBA_BASE]
        je      atk_yes
        add     esi,HBA_LEN
        dec     ecx
        jmp     atk_hba
atk_no:
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret
atk_yes:
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret

;---------------------------------------------------------------------
; aha_pcione - EBX = a BusLogic MultiMaster's PCI address.  Bring it up
;              at the port its BAR0 names, and note its ISA port for
;              aha_taken.  All registers kept.
;---------------------------------------------------------------------
aha_pcione:
        pushad
        mov     al,PCIR_BAR0
        call    pci_read
        test    al,1                    ; an I/O register, or not ours
        jz      apc_out
        and     eax,0FFFCh
        jz      apc_out
        mov     edx,eax

        ; answering I/O, and allowed to master the bus: a card whose
        ; BIOS is switched off may have been left without either
        push    edx
        mov     al,PCIR_COMMAND
        call    pci_read
        movzx   ecx,ax                  ; the command half only: the
        or      ecx,PCIC_IOEN OR PCIC_BUSMASTER ; status half clears
        mov     al,PCIR_COMMAND                 ; what is written to it
        call    pci_write
        pop     edx

        mov     byte ptr [aha_onpci],1
        call    aha_one
        mov     byte ptr [aha_onpci],0
        jc      apc_out

        ; where else it answers
        mov     [aha_base],dx
        mov     byte ptr [aha_cbuf],AHK_BLPCI
        mov     esi,OFFSET aha_cbuf
        mov     ecx,1
        mov     edx,4
        call    aha_cmd
        jc      apc_out
        movzx   eax,byte ptr [aha_rep]
        cmp     eax,6
        jae     apc_out                 ; 6 and 7: switched off
        mov     ax,word ptr [aha_ports+eax*2]
        mov     esi,OFFSET aha_skip
apc_slot:
        cmp     word ptr [esi],0
        je      apc_put
        add     esi,2
        cmp     esi,OFFSET aha_skip + 8
        jb      apc_slot
        jmp     short apc_out
apc_put:
        mov     [esi],ax
apc_out:
        popad
        ret

;=====================================================================
; aha_one - DX = a port.  If an AHA-154x or a BusLogic card answers
;           there, and it is nobody else's, bring it up and register
;           it.  CF if not.  All registers kept.
;=====================================================================
aha_one:
        pushad
        mov     [aha_base],dx
        mov     eax,[scs_nhba]
        cmp     eax,SCSI_MAXHBA
        jae     ao_no
        imul    eax,eax,AHB_LEN
        lea     ebp,[aha_blk+eax]       ; EBP = its mailboxes and CCB

        ; COULD IT BE ONE?  Read only, and nothing written until the
        ; answers look right: 330h is also where an MPU-401 lives, and
        ; a card that is not this one must not be sent a reset
        in      al,dx
        cmp     al,0FFh
        je      ao_no
        test    al,02h                  ; reserved, always clear
        jnz     ao_no
        add     dx,AHR_INTR
        in      al,dx
        test    al,70h                  ; reserved, always clear
        jnz     ao_no

        ; AN ASPI MANAGER'S?  See the header.
        test    byte ptr [scs_pflags],SPF_ASPI
        jz      ao_ours
        mov     dx,[aha_base]
        in      al,dx
        test    al,AHS_INIT
        jnz     ao_ours                 ; nobody has given it mailboxes
        test    al,AHS_IDLE
        jz      ao_no                   ; busy: somebody is using it now
        mov     byte ptr [aha_cbuf],AHK_RETSETUP
        mov     byte ptr [aha_cbuf+1],8
        mov     esi,OFFSET aha_cbuf
        mov     ecx,2
        mov     edx,8
        call    aha_cmd
        jc      ao_no                   ; cannot tell: leave it alone
        cmp     byte ptr [aha_rep+4],0  ; how many mailboxes it has
        jne     ao_no                   ; some: the manager's
ao_ours:
        call    aha_reset
        jc      ao_no

        ; WHAT IT IS.  Four bytes of board id, which also proves the
        ; command port works; 'C' first is an AHA-1740 imitating a
        ; 1542, which Linux will not trust and neither does this
        mov     byte ptr [aha_cbuf],AHK_INQUIRY
        mov     esi,OFFSET aha_cbuf
        mov     ecx,1
        mov     edx,4
        call    aha_cmd
        jc      ao_no
        cmp     byte ptr [aha_rep],43h
        je      ao_no

        call    aha_unlock

        mov     byte ptr [aha_cbuf],AHK_RETCONF
        mov     esi,OFFSET aha_cbuf
        mov     ecx,1
        mov     edx,3
        call    aha_cmd
        jc      ao_no
        mov     al,byte ptr [aha_rep]
        mov     byte ptr [aha_dmabit],al
        mov     al,byte ptr [aha_rep+1]
        mov     byte ptr [aha_irqbit],al
        mov     al,byte ptr [aha_rep+2]
        and     al,07h
        mov     byte ptr [aha_hostid],al

        ; ADAPTEC OR BUSLOGIC?  Only BusLogic knows 84h; an Adaptec
        ; card calls it invalid and finishes, which aha_cmd sees
        mov     byte ptr [aha_kind],SHK_AHA
        mov     byte ptr [aha_cbuf],AHK_BLVER3
        mov     esi,OFFSET aha_cbuf
        mov     ecx,1
        mov     edx,1
        call    aha_cmd
        jc      ao_kind
        mov     byte ptr [aha_kind],SHK_BUSLOGIC
ao_kind:
        call    aha_cascade
        call    aha_mbinit              ; EBP = the block
        jc      ao_no

        ; the record SCSI.INC keeps
        mov     edi,OFFSET aha_tmpl
        push    edi
        xor     eax,eax
        mov     ecx,HBA_LEN
        cld
        rep     stosb
        pop     edi
        mov     dword ptr [edi+HBA_EXEC],OFFSET aha_exec
        mov     al,byte ptr [aha_kind]
        mov     [edi+HBA_KIND],al
        mov     al,byte ptr [aha_hostid]
        mov     [edi+HBA_ID],al
        mov     al,SHF_BOUNCE
        cmp     byte ptr [aha_onpci],0
        je      ao_isa
        or      al,SHF_PCI
ao_isa:
        mov     [edi+HBA_FLAGS],al
        mov     ax,[aha_base]
        mov     [edi+HBA_BASE],ax
        call    aha_irqdma              ; -> AL = IRQ, AH = DMA, FFh = none
        mov     [edi+HBA_IRQ],al
        mov     [edi+HBA_DMA],ah
        mov     dword ptr [edi+HBA_MAXX],SCSI_BUFLEN
        mov     [edi+HBA_PRIV],ebp
        mov     esi,OFFSET aha_nmaha
        cmp     byte ptr [aha_kind],SHK_BUSLOGIC
        jne     ao_name
        mov     esi,OFFSET aha_nmbl
ao_name:
        push    edi
        add     edi,HBA_NAME
        mov     ecx,16
        rep     movsb
        pop     edi
        call    scs_addhba
        jc      ao_no
        popad
        clc
        ret
ao_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; aha_irqdma - the RETCONF bits in aha_irqbit and aha_dmabit as
;              numbers.  -> AL = IRQ, AH = DMA channel; FFh for none.
;              Everything else kept.
;---------------------------------------------------------------------
aha_irqdma:
        push    ecx
        mov     al,0FFh
        mov     cl,byte ptr [aha_irqbit]        ; bit 0 is IRQ 9
        or      cl,cl
        jz      aid_dma
        mov     al,9
aid_irq:
        shr     cl,1
        jc      aid_dma
        inc     al
        jmp     short aid_irq
aid_dma:
        mov     ah,0FFh
        cmp     byte ptr [aha_onpci],0
        jne     aid_out                 ; a PCI card has no channel
        mov     cl,byte ptr [aha_dmabit]        ; bit n is channel n
        or      cl,cl
        jz      aid_out
        mov     ah,0
aid_ch:
        shr     cl,1
        jc      aid_out
        inc     ah
        jmp     short aid_ch
aid_out:
        pop     ecx
        ret

;---------------------------------------------------------------------
; aha_cascade - put the card's ISA DMA channel into cascade mode, which
;               is how a bus master is let onto the bus.  The card's
;               BIOS does this at POST when it is switched on and an
;               ASPI manager does it when it loads; with neither, it is
;               this or nothing moves.  Nothing for a PCI card.  All
;               registers kept.
;---------------------------------------------------------------------
aha_cascade:
        push    eax
        cmp     byte ptr [aha_onpci],0
        jne     acs_out
        mov     ah,byte ptr [aha_dmabit]
        call    vdma_keep               ; the card's, not a DOS box's to mask
        test    ah,01h
        jz      acs_hi
        mov     al,0C0h                 ; channel 0: the first controller
        out     0Bh,al
        xor     al,al
        out     0Ah,al
        jmp     short acs_out
acs_hi:
        mov     al,0C1h                 ; 5, 6 and 7: the second, where
        test    ah,20h                  ; they are 1, 2 and 3
        jnz     acs_set
        mov     al,0C2h
        test    ah,40h
        jnz     acs_set
        mov     al,0C3h
        test    ah,80h
        jz      acs_out
acs_set:
        out     0D6h,al                 ; mode: cascade
        and     al,03h
        out     0D4h,al                 ; and unmasked
acs_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; aha_unlock - the 1542C's mailbox lock.  With extended translation on,
;              its BIOS locks the mailbox interface until the driver
;              reads the lock code (28h) and hands it back (29h).
;              Cards without it call 28h invalid, which is the end of
;              it.  [aha_base] = the card.  All registers kept.
;---------------------------------------------------------------------
aha_unlock:
        pushad
        mov     byte ptr [aha_cbuf],AHK_EXTBIOS
        mov     esi,OFFSET aha_cbuf
        mov     ecx,1
        mov     edx,2
        call    aha_cmd
        jc      aul_out
        test    byte ptr [aha_rep],08h
        jnz     aul_open
        cmp     byte ptr [aha_rep+1],0
        je      aul_out
aul_open:
        mov     byte ptr [aha_cbuf],AHK_MBENABLE
        mov     byte ptr [aha_cbuf+1],0
        mov     al,byte ptr [aha_rep+1]
        mov     byte ptr [aha_cbuf+2],al
        mov     esi,OFFSET aha_cbuf
        mov     ecx,3
        xor     edx,edx
        call    aha_cmd
aul_out:
        popad
        ret

;---------------------------------------------------------------------
; aha_reset - soft reset [aha_base], and expect it to come back asking
;             for mailboxes, idle, and with nothing else to say.  CF if
;             it does not.  All registers kept.
;---------------------------------------------------------------------
aha_reset:
        pushad
        mov     dx,[aha_base]
        mov     al,AHC_SRST OR AHC_IRST
        out     dx,al
        call    aha_settle
        mov     ecx,AHW_RESET
ars_wait:
        in      al,dx
        and     al,AHS_MASK
        cmp     al,AHS_INIT OR AHS_IDLE
        je      ars_up
        dec     ecx
        jnz     ars_wait
        popad
        stc
        ret
ars_up:
        add     dx,AHR_INTR             ; and no interrupt from the reset
        in      al,dx
        test    al,AHI_MASK
        jnz     ars_bad
        popad
        clc
        ret
ars_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; aha_settle - about 20ms, in reads of port 80h.  All kept.
;---------------------------------------------------------------------
aha_settle:
        push    eax
        push    ecx
        mov     ecx,AHW_SETTLE
ast_lp:
        in      al,80h
        loop    ast_lp
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; aha_mbinit - one out-mailbox and one in-mailbox, at EBP, given to
;              [aha_base].  CF if the card would not take them.  All
;              registers kept.
;---------------------------------------------------------------------
aha_mbinit:
        pushad
        xor     eax,eax
        mov     [ebp+AHB_MBO],eax
        mov     [ebp+AHB_MBI],eax
        mov     byte ptr [aha_cbuf],AHK_MBINIT
        mov     byte ptr [aha_cbuf+1],1
        mov     eax,ebp                 ; linear is physical, and PMIO
        mov     byte ptr [aha_cbuf+4],al        ; is below 16MB
        mov     byte ptr [aha_cbuf+3],ah
        shr     eax,16
        mov     byte ptr [aha_cbuf+2],al
        mov     esi,OFFSET aha_cbuf
        mov     ecx,5
        xor     edx,edx
        call    aha_cmd
        jc      amb_bad
        mov     dx,[aha_base]
        in      al,dx
        test    al,AHS_INIT
        jnz     amb_bad
        popad
        clc
        ret
amb_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; aha_cmd - one command to [aha_base].  ESI -> it and its parameters,
;           ECX bytes; EDX = reply bytes wanted, into aha_rep.  CF if
;           the card would not take it, did not answer, or called it
;           invalid.  All registers kept.
;
; A COMMAND THE CARD DOES NOT KNOW ENDS AT ONCE: it sets INVDCMD and
; HACC and takes nothing more.  So both loops watch HACC as well as
; the port they are waiting on, and an unknown command - the 84h that
; tells BusLogic from Adaptec, the 28h only a 1542C has - costs a few
; reads rather than the whole wait.
;---------------------------------------------------------------------
aha_cmd:
        pushad
        call    aha_irst                ; no stale HACC taken for this one
        mov     ebp,edx
        cld
acm_out:
        lodsb
        call    aha_put
        jc      acm_bad
        loop    acm_out
        mov     edi,OFFSET aha_rep
        mov     ecx,ebp
        jecxz   acm_wait
acm_in:
        call    aha_get
        jc      acm_bad
        stosb
        loop    acm_in
acm_wait:
        mov     dx,[aha_base]
        add     dx,AHR_INTR
        mov     ecx,AHW_BYTE
acm_hacc:
        in      al,dx
        test    al,AHI_HACC
        jnz     acm_done
        dec     ecx
        jnz     acm_hacc
        jmp     short acm_bad
acm_done:
        mov     dx,[aha_base]
        in      al,dx
        test    al,AHS_INVDCMD
        jnz     acm_bad
        call    aha_irst
        popad
        clc
        ret
acm_bad:
        call    aha_irst
        popad
        stc
        ret

;---------------------------------------------------------------------
; aha_put - AL to the command port once there is room.  CF if there
;           never is, or if the card has already finished (HACC): the
;           command was refused and this byte would be taken for a new
;           one.  All registers kept.
;---------------------------------------------------------------------
aha_put:
        push    ecx
        push    edx
        push    eax
        mov     ecx,AHW_BYTE
apu_wait:
        mov     dx,[aha_base]
        add     dx,AHR_INTR
        in      al,dx
        test    al,AHI_HACC
        jnz     apu_bad
        mov     dx,[aha_base]
        in      al,dx
        test    al,AHS_CDF
        jz      apu_room
        dec     ecx
        jnz     apu_wait
apu_bad:
        pop     eax
        pop     edx
        pop     ecx
        stc
        ret
apu_room:
        pop     eax
        inc     dx                      ; AHR_DATA
        out     dx,al
        pop     edx
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; aha_get - the next reply byte from [aha_base] -> AL.  CF if none
;           comes before the card says it has finished.  All other
;           registers kept.
;---------------------------------------------------------------------
aha_get:
        push    ecx
        push    edx
        mov     ecx,AHW_BYTE
agt_wait:
        mov     dx,[aha_base]
        in      al,dx
        test    al,AHS_DF
        jnz     agt_have
        add     dx,AHR_INTR
        in      al,dx
        test    al,AHI_HACC
        jnz     agt_bad
        dec     ecx
        jnz     agt_wait
agt_bad:
        pop     edx
        pop     ecx
        stc
        ret
agt_have:
        inc     dx                      ; AHR_DATA
        in      al,dx
        pop     edx
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; aha_irst - clear [aha_base]'s interrupt flags.  All kept.
;---------------------------------------------------------------------
aha_irst:
        push    eax
        push    edx
        mov     dx,[aha_base]
        mov     al,AHC_IRST
        out     dx,al
        pop     edx
        pop     eax
        ret

;=====================================================================
; aha_exec - HBA_EXEC.  EBX -> the adapter, EDI -> a request.  CF
;            unless the target said GOOD.  All registers kept.
;
; SRQ_BUF is below 16MB by the time it gets here: the adapter is
; flagged SHF_BOUNCE and scs_run has swapped in SCSI_BUF.
;=====================================================================
aha_exec:
        pushad
        mov     dx,[ebx+HBA_BASE]
        mov     [aha_base],dx
        mov     ebp,[ebx+HBA_PRIV]

        ; MAILBOXES TAKEN AWAY?  See the header: a reset leaves the
        ; card asking for them again, and it gets them back
        in      al,dx
        test    al,AHS_INIT
        jz      ahx_mbok
        call    aha_mbinit
        jc      ahx_err
ahx_mbok:
        lea     esi,[ebp+AHB_CCB]       ; ESI -> the CCB
        push    edi
        mov     edi,esi
        xor     eax,eax
        mov     ecx,AHB_LEN - AHB_CCB
        cld
        rep     stosb
        pop     edi

        ; DIRECTION BITS ZERO: the command decides which way the data
        ; goes, and the length is not checked - a reply shorter than
        ; the room for it is normal and not an error
        mov     al,[edi+SRQ_TARGET]
        shl     al,5
        mov     cl,[edi+SRQ_LUN]
        and     cl,07h
        or      al,cl
        mov     [esi+CCB_IDLUN],al
        mov     al,[edi+SRQ_CDBLEN]
        mov     [esi+CCB_CDBLEN],al
        mov     byte ptr [esi+CCB_RSALEN],CCB_SENSELEN
        mov     eax,[edi+SRQ_COUNT]
        mov     [esi+CCB_DLEN+2],al
        mov     [esi+CCB_DLEN+1],ah
        shr     eax,16
        mov     [esi+CCB_DLEN],al
        mov     eax,[edi+SRQ_BUF]
        mov     [esi+CCB_DPTR+2],al
        mov     [esi+CCB_DPTR+1],ah
        shr     eax,16
        mov     [esi+CCB_DPTR],al
        push    esi
        push    edi
        movzx   ecx,byte ptr [edi+SRQ_CDBLEN]
        lea     eax,[esi+CCB_CDB]
        lea     esi,[edi+SRQ_CDB]
        mov     edi,eax
        rep     movsb
        pop     edi
        pop     esi

        ; the out-mailbox: start this CCB
        mov     eax,esi
        mov     [ebp+AHB_MBO+3],al
        mov     [ebp+AHB_MBO+2],ah
        shr     eax,16
        mov     [ebp+AHB_MBO+1],al
        mov     byte ptr [ebp+AHB_MBI],0
        mov     byte ptr [ebp+AHB_MBO],1

        call    aha_irst
        mov     al,AHK_START
        call    aha_put
        jc      ahx_hung

        ; and wait for the in-mailbox.  The read is the clock.
        mov     dx,[aha_base]
        add     dx,AHR_INTR
        mov     ecx,AHW_IO
ahx_wait:
        cmp     byte ptr [ebp+AHB_MBI],0
        jne     ahx_done
        in      al,dx
        dec     ecx
        jnz     ahx_wait
        jmp     short ahx_hung

ahx_done:
        mov     al,[ebp+AHB_MBI]
        mov     byte ptr [ebp+AHB_MBI],0
        call    aha_irst
        cmp     al,AHM_ABORTED
        je      ahx_err
        cmp     al,AHM_NOTFOUND
        je      ahx_err
        mov     al,[esi+CCB_TSTAT]
        mov     [edi+SRQ_TSTAT],al
        mov     al,[esi+CCB_HASTAT]
        or      al,al
        jz      ahx_hok
        cmp     al,AHH_UNDER            ; a short reply
        je      ahx_hok
        cmp     al,AHH_SELTO
        jne     ahx_err
        mov     byte ptr [edi+SRQ_HSTAT],SRH_NODEV
        jmp     short ahx_fail
ahx_hok:
        mov     byte ptr [edi+SRQ_HSTAT],SRH_OK
        cmp     byte ptr [edi+SRQ_TSTAT],SCSI_ST_CHECK
        jne     ahx_nosense
        ; the sense the card fetched, after the command block
        push    esi
        push    edi
        movzx   eax,byte ptr [esi+CCB_CDBLEN]
        lea     esi,[esi+CCB_CDB+eax]
        add     edi,SRQ_SENSE
        mov     ecx,CCB_SENSELEN
        rep     movsb
        pop     edi
        pop     esi
        mov     byte ptr [edi+SRQ_SLEN],CCB_SENSELEN
ahx_nosense:
        cmp     byte ptr [edi+SRQ_TSTAT],SCSI_ST_GOOD
        jne     ahx_fail
        mov     eax,[edi+SRQ_COUNT]
        mov     [edi+SRQ_GOT],eax
        popad
        clc
        ret

ahx_hung:
        ; IT NEVER ANSWERED.  Reset the card and the bus under it, so
        ; that the next request finds both idle, and give the card its
        ; mailboxes back
        mov     byte ptr [edi+SRQ_HSTAT],SRH_TIMEOUT
        mov     dx,[aha_base]
        mov     al,AHC_SRST OR AHC_IRST OR AHC_SCRST
        out     dx,al
        call    aha_settle
        mov     ecx,AHW_RESET
ahx_back:
        in      al,dx
        test    al,AHS_IDLE
        jnz     ahx_up
        dec     ecx
        jnz     ahx_back
ahx_up:
        call    aha_mbinit
        jmp     short ahx_fail
ahx_err:
        mov     byte ptr [edi+SRQ_HSTAT],SRH_ERROR
ahx_fail:
        popad
        stc
        ret

;=====================================================================
; Data
;=====================================================================
aha_ports   dw 330h, 334h, 230h, 234h, 130h, 134h, 0    ; the order BusLogic
                                                        ; numbers them in, too
aha_skip    dw 4 dup (0)                ; ports a PCI card also answers at
aha_base    dw 0                        ; the card being spoken to
aha_onpci   db 0                        ; ...found on the PCI bus
aha_kind    db 0                        ; SHK_AHA or SHK_BUSLOGIC
aha_hostid  db 0
aha_dmabit  db 0                        ; RETCONF's bytes
aha_irqbit  db 0
aha_cbuf    db 8 dup (0)                ; a command being sent
aha_rep     db 16 dup (0)               ; ...and what came back
aha_nmaha   db 'Adaptec 154x    '
aha_nmbl    db 'BusLogic        '
aha_tmpl    db HBA_LEN dup (0)
            ALIGN 4
aha_blk     db SCSI_MAXHBA * AHB_LEN dup (0)    ; mailboxes and CCBs
